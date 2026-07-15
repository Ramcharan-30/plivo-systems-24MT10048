/*
 * Receiver — Ultra-Low Latency Kernel Optimization
 *
 * Implements a cache-line aligned O(1) ring buffer.
 * Uses epoll_wait() and vectorized recvmmsg() to pull datagram bursts 
 * with zero-blocking overhead.
 * Employs a 50us busy-wait using CLOCK_MONOTONIC for precise playout.
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

#define PAYLOAD_LEN 160
#define RING_SIZE 2048
#define MAX_FRAMES 16000
#define VLEN 32  /* Vectorized batch size for recvmmsg */

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

/* Cache-line aligned struct to prevent false sharing */
struct Frame {
    uint8_t payload[PAYLOAD_LEN];
    bool dispatched;
} __attribute__((aligned(64)));

static struct Frame ring[RING_SIZE] __attribute__((aligned(64)));
static int out_fd;
static struct sockaddr_in player;

static inline double get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Dispatch frame with 50us monotonic precision */
static inline void dispatch_frame(uint32_t seq, const uint8_t *payload) {
    if (seq >= MAX_FRAMES) return;
    
    uint32_t idx = seq % RING_SIZE;
    if (ring[idx].dispatched) return;
    ring[idx].dispatched = true;

    struct PktSingle out;
    out.seq = htonl(seq); /* Robust network byte order safety */
    memcpy(out.payload, payload, PAYLOAD_LEN);

    /* Target playout: 50us after arrival/recovery to bypass scheduling latency */
    double target_mono = get_monotonic_time() + 0.000050;
    
    /* Hyper-precise busy-wait spin loop */
    while (get_monotonic_time() < target_mono) {
        __asm__ volatile("pause" ::: "memory");
    }

    sendto(out_fd, &out, sizeof(out), 0,
           (struct sockaddr *)&player, sizeof(player));
}

int main(void) {
    memset(ring, 0, sizeof(ring));

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

    /* Setup epoll for explicit event loop */
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = in_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, in_fd, &ev);
    struct epoll_event events[1];

    /* Setup for recvmmsg vectorized networking */
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
        /* Wait for incoming UDP packets via epoll */
        int nfds = epoll_wait(epfd, events, 1, -1);
        if (nfds < 0) continue;

        /* Pull maximum datagrams out of kernel without blocking */
        int vlen = recvmmsg(in_fd, msgs, VLEN, MSG_DONTWAIT, NULL);
        if (vlen < 0) continue;

        for (int i = 0; i < vlen; i++) {
            ssize_t n = msgs[i].msg_len;
            if (n < (ssize_t)sizeof(struct PktSingle)) continue;

            uint8_t *buf = bufs[i];
            struct PktSingle *hdr = (struct PktSingle *)buf;
            
            /* Native byte order processing */
            uint32_t seq = ntohl(hdr->seq);

            /* Dispatch Current Frame (N) */
            if (seq < MAX_FRAMES) {
                dispatch_frame(seq, buf + 4);
            }

            /* Dispatch Redundancy (Frame N-1) */
            if (n == (ssize_t)sizeof(struct PktDual) && seq > 0) {
                uint32_t prev_seq = seq - 1;
                if (prev_seq < MAX_FRAMES) {
                    struct PktDual *dual = (struct PktDual *)buf;
                    dispatch_frame(prev_seq, dual->payload_prev);
                }
            }
        }
    }
    
    close(epfd);
    return 0;
}
