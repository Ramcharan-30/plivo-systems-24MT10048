/*
 * Receiver — Ultra-Low Latency Deadline Optimization
 *
 * Single-threaded architecture merging epoll_wait and playout.
 * Transitions from blocking epoll() to an active recvmmsg() spin-yield loop 
 * in the final 3 milliseconds before a deadline to completely bypass OS 
 * thread-wake scheduling latency.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define PAYLOAD_LEN 160
#define RING_SIZE 2048
#define MAX_FRAMES 16000
#define VLEN 32

#pragma pack(push, 1)
struct PktSingle {
    uint32_t seq;
    uint8_t payload[PAYLOAD_LEN];
} __attribute__((packed));

struct PktDual {
    uint32_t seq;
    uint8_t payload_curr[PAYLOAD_LEN];
    uint8_t payload_prev[PAYLOAD_LEN];
} __attribute__((packed));
#pragma pack(pop)

struct Frame {
    uint8_t payload[PAYLOAD_LEN];
    bool present;
    bool dispatched;
} __attribute__((aligned(64)));

static struct Frame ring[RING_SIZE] __attribute__((aligned(64)));
static int out_fd;
static struct sockaddr_in player;

static double global_t0 = 0.0;
static double max_delay_ms = 60.0;

static inline double get_real_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline void process_received_burst(int in_fd, struct mmsghdr *msgs, uint8_t bufs[][2048]) {
    int vlen;
    do {
        vlen = recvmmsg(in_fd, msgs, VLEN, MSG_DONTWAIT, NULL);
        if (vlen <= 0) break;

        for (int i = 0; i < vlen; i++) {
            ssize_t n = msgs[i].msg_len;
            if (n < (ssize_t)sizeof(struct PktSingle)) continue;

            uint8_t *buf = bufs[i];
            struct PktSingle *hdr = (struct PktSingle *)buf;
            uint32_t seq = ntohl(hdr->seq);

            /* Primary Frame */
            if (seq < MAX_FRAMES && !ring[seq % RING_SIZE].present) {
                memcpy(ring[seq % RING_SIZE].payload, buf + 4, PAYLOAD_LEN);
                ring[seq % RING_SIZE].present = true;
            }

            /* Redundant Sliding-Window FEC (Frame N-1) */
            if (n == (ssize_t)sizeof(struct PktDual) && seq > 0) {
                uint32_t prev_seq = seq - 1;
                if (prev_seq < MAX_FRAMES && !ring[prev_seq % RING_SIZE].present) {
                    struct PktDual *dual = (struct PktDual *)buf;
                    memcpy(ring[prev_seq % RING_SIZE].payload, dual->payload_prev, PAYLOAD_LEN);
                    ring[prev_seq % RING_SIZE].present = true;
                }
            }
        }
    } while (vlen == VLEN); /* Completely drain kernel buffer */
}

int main(void) {
    memset(ring, 0, sizeof(ring));
    
    const char *env_t0 = getenv("T0");
    if (env_t0) global_t0 = atof(env_t0);
    
    const char *env_delay = getenv("DELAY_MS");
    if (env_delay) max_delay_ms = atof(env_delay);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player, 0, sizeof(player));
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = in_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, in_fd, &ev);
    struct epoll_event events[1];

    struct mmsghdr msgs[VLEN];
    struct iovec iovecs[VLEN];
    uint8_t bufs[VLEN][2048];

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < VLEN; i++) {
        iovecs[i].iov_base = bufs[i];
        iovecs[i].iov_len = sizeof(bufs[i]);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* Wait for T0 initialization */
    while (global_t0 == 0.0) {
        usleep(1000);
        if (getenv("T0")) global_t0 = atof(getenv("T0"));
    }

    uint32_t current_frame = 0;

    while (current_frame < MAX_FRAMES) {
        double now = get_real_time();
        
        /* Strict target deadline for the current frame */
        double absolute_deadline = global_t0 + (current_frame * 0.020) + (max_delay_ms / 1000.0);
        double cutoff = absolute_deadline - 0.000010; /* 10us UDP transmit margin */
        
        uint32_t idx = current_frame % RING_SIZE;

        if (ring[idx].present || now >= cutoff) {
            /* Dispatch if present, or if deadline expired */
            if (ring[idx].present && !ring[idx].dispatched) {
                struct PktSingle out;
                out.seq = htonl(current_frame);
                memcpy(out.payload, ring[idx].payload, PAYLOAD_LEN);
                
                sendto(out_fd, &out, sizeof(out), 0,
                       (struct sockaddr *)&player, sizeof(player));
                
                ring[idx].dispatched = true;
                ring[idx].present = false; /* Free slot */
            }
            current_frame++;
            continue;
        }

        /* We are waiting for the frame. Determine strategy based on time to deadline */
        double diff = cutoff - now;
        
        if (diff > 0.003) {
            /* 
             * Coarse mode: > 3ms away. 
             * Safe to block in epoll_wait to save CPU cycles.
             * Calculate timeout in milliseconds.
             */
            int wait_ms = (int)((diff - 0.003) * 1000.0);
            if (wait_ms < 1) wait_ms = 1;
            
            epoll_wait(epfd, events, 1, wait_ms);
            process_received_burst(in_fd, msgs, bufs);
        } else {
            /* 
             * Hyper-precise mode: < 3ms away. 
             * Actively pull from socket and spin-yield to entirely bypass 
             * OS scheduler thread-wake latency. 
             */
            process_received_burst(in_fd, msgs, bufs);
            sched_yield();
        }
    }
    
    close(epfd);
    return 0;
}
