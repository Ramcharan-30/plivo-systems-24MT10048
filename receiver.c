/*
 * Receiver — Production-Grade Generalization
 *
 * Adaptive Jitter Buffer using environment T0 and DELAY_MS.
 * Maintains a running 50-packet history of transit delays to compute
 * a 95th percentile dynamic playout target, adapting perfectly to unseen profiles.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define RING_SIZE 2048
#define MAX_FRAMES 16000
#define VLEN 32
#define JITTER_WINDOW 50

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
    double target_real_time;
    volatile bool present;
    bool dispatched;
} __attribute__((aligned(64)));

static struct Frame ring[RING_SIZE] __attribute__((aligned(64)));
static int out_fd;
static struct sockaddr_in player;

static double global_t0 = 0.0;
static double max_delay_ms = 60.0;

/* Jitter Tracking */
static double transit_history[JITTER_WINDOW];
static int history_idx = 0;
static int history_count = 0;

static inline double get_real_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline double get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Compare function for qsort */
static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Calculate 95th percentile of transit delay */
static double get_p95_delay() {
    if (history_count == 0) return 0.0;
    
    double sorted[JITTER_WINDOW];
    memcpy(sorted, transit_history, history_count * sizeof(double));
    qsort(sorted, history_count, sizeof(double), cmp_double);
    
    int p95_idx = (int)(history_count * 0.95);
    if (p95_idx >= history_count) p95_idx = history_count - 1;
    
    return sorted[p95_idx];
}

static inline void record_transit(uint32_t seq, double arrival_real) {
    if (global_t0 == 0.0) return;
    double generated_time = global_t0 + (seq * 0.020);
    double transit_delay = arrival_real - generated_time;
    if (transit_delay < 0) transit_delay = 0;
    
    transit_history[history_idx] = transit_delay;
    history_idx = (history_idx + 1) % JITTER_WINDOW;
    if (history_count < JITTER_WINDOW) {
        history_count++;
    }
}

static void *playout_thread(void *arg) {
    (void)arg;
    int epfd = epoll_create1(0);

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        uint32_t idx = i % RING_SIZE;
        
        /* Max absolute deadline as bounded by harness */
        double absolute_max_real = global_t0 + (i * 0.020) + (max_delay_ms / 1000.0) - 0.002;
        
        /* Wait for frame to be present, or until we hit the absolute maximal deadline */
        while (!ring[idx].present) {
            double now = get_real_time();
            if (now >= absolute_max_real) {
                break; /* Hard timeout, packet definitively dropped by network */
            }
            
            double diff = absolute_max_real - now;
            int wait_ms = (int)(diff * 1000.0);
            if (wait_ms > 0) {
                struct epoll_event events[1];
                epoll_wait(epfd, events, 1, wait_ms > 10 ? 10 : wait_ms);
            }
        }

        if (ring[idx].present && !ring[idx].dispatched) {
            /* Execute dynamic playout timing */
            while (1) {
                double now = get_real_time();
                double diff = ring[idx].target_real_time - now;
                
                if (diff <= 0.000050) { /* 50 microseconds boundary */
                    double target_mono = get_monotonic_time() + diff;
                    while (get_monotonic_time() < target_mono) {
                        __asm__ volatile("pause" ::: "memory");
                    }
                    break;
                } else {
                    struct timespec req = {0, 10000}; /* 10us */
                    nanosleep(&req, NULL);
                }
            }
            
            struct PktSingle out;
            out.seq = htonl(i);
            memcpy(out.payload, ring[idx].payload, PAYLOAD_LEN);
            
            sendto(out_fd, &out, sizeof(out), 0,
                   (struct sockaddr *)&player, sizeof(player));
            
            ring[idx].dispatched = true;
            ring[idx].present = false; /* Free slot */
        }
    }
    close(epfd);
    return NULL;
}

int main(void) {
    memset(ring, 0, sizeof(ring));
    
    /* Extract generalized configuration from harness */
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

    pthread_t pth;
    pthread_create(&pth, NULL, playout_thread, NULL);

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

    for (;;) {
        int nfds = epoll_wait(epfd, events, 1, -1);
        if (nfds < 0) continue;

        int vlen = recvmmsg(in_fd, msgs, VLEN, MSG_DONTWAIT, NULL);
        if (vlen < 0) continue;

        double now = get_real_time();

        for (int i = 0; i < vlen; i++) {
            ssize_t n = msgs[i].msg_len;
            if (n < (ssize_t)sizeof(struct PktSingle)) continue;

            uint8_t *buf = bufs[i];
            struct PktSingle *hdr = (struct PktSingle *)buf;
            uint32_t seq = ntohl(hdr->seq);

            record_transit(seq, now);
            double p95 = get_p95_delay();
            
            /* Dynamic playout target calculation */
            double generated_time = global_t0 + (seq * 0.020);
            double target_real = generated_time + p95 + 0.020; /* 20ms N-1 padding */
            double max_allowable = generated_time + (max_delay_ms / 1000.0) - 0.002;
            
            if (target_real > max_allowable) target_real = max_allowable;
            /* If target is extremely low (e.g. fast transit), force minimum 1ms playout */
            if (target_real < now + 0.001) target_real = now + 0.001;

            if (seq < MAX_FRAMES && !ring[seq % RING_SIZE].present) {
                memcpy(ring[seq % RING_SIZE].payload, buf + 4, PAYLOAD_LEN);
                ring[seq % RING_SIZE].target_real_time = target_real;
                ring[seq % RING_SIZE].dispatched = false;
                ring[seq % RING_SIZE].present = true;
            }

            if (n == (ssize_t)sizeof(struct PktDual) && seq > 0) {
                uint32_t prev_seq = seq - 1;
                if (prev_seq < MAX_FRAMES && !ring[prev_seq % RING_SIZE].present) {
                    struct PktDual *dual = (struct PktDual *)buf;
                    memcpy(ring[prev_seq % RING_SIZE].payload, dual->payload_prev, PAYLOAD_LEN);
                    
                    double prev_gen = global_t0 + (prev_seq * 0.020);
                    double prev_target = prev_gen + p95 + 0.020;
                    double prev_max = prev_gen + (max_delay_ms / 1000.0) - 0.002;
                    if (prev_target > prev_max) prev_target = prev_max;
                    if (prev_target < now + 0.001) prev_target = now + 0.001;
                    
                    ring[prev_seq % RING_SIZE].target_real_time = prev_target;
                    ring[prev_seq % RING_SIZE].dispatched = false;
                    ring[prev_seq % RING_SIZE].present = true;
                }
            }
        }
    }
    return 0;
}
