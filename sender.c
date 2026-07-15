/*
 * Sender — Extreme Latency Optimization (N-1 Redundancy)
 *
 * Maximizes the 2.0x Bandwidth Budget by packing Frame N and Frame N-1
 * into a single zero-copy UDP datagram using strict struct packing.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160

#pragma pack(push, 1)
struct PktSingle {
    uint32_t seq;
    uint8_t payload[PAYLOAD_LEN];
};

struct PktDual {
    uint32_t seq;
    uint8_t payload_curr[PAYLOAD_LEN];
    uint8_t payload_prev[PAYLOAD_LEN];
};
#pragma pack(pop)

int main(void)
{
    const char *dur_str = getenv("DURATION_S");
    double duration = dur_str ? atof(dur_str) : 30.0;
    int n_frames = (int)(duration * 1000.0 / 20.0);
    
    /* 
     * Cap calculation for exactly 2.0x overhead.
     * raw = n_frames * 160
     * cap = 2 * raw
     * dual_size = 324, single_size = 164
     * We need: dual_count * 324 + single_count * 164 <= cap
     *          dual_count + single_count = n_frames
     * For 1500 frames, this allows exactly 1462 dual packets and 38 singles.
     */
    int raw_bytes = n_frames * PAYLOAD_LEN;
    int max_bytes = 2 * raw_bytes;
    int max_dual_packets = (max_bytes - (int)sizeof(struct PktSingle) * n_frames) / 
                           (int)(sizeof(struct PktDual) - sizeof(struct PktSingle));

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in src_addr = {0};
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(47010);
    src_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("bind 47010");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t prev_payload[PAYLOAD_LEN];
    int dual_sent = 0;
    uint8_t buf[2048];

    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < (ssize_t)sizeof(struct PktSingle)) continue;

        struct PktSingle *in_pkt = (struct PktSingle *)buf;
        uint32_t host_seq = ntohl(in_pkt->seq);

        if (host_seq == 0 || dual_sent >= max_dual_packets) {
            /* Send single packet */
            sendto(out_fd, buf, sizeof(struct PktSingle), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
        } else {
            /* Zero-copy style inline struct assignment where possible */
            struct PktDual dual;
            dual.seq = in_pkt->seq; /* Keep big-endian */
            memcpy(dual.payload_curr, in_pkt->payload, PAYLOAD_LEN);
            memcpy(dual.payload_prev, prev_payload, PAYLOAD_LEN);
            
            sendto(out_fd, &dual, sizeof(struct PktDual), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
            dual_sent++;
        }

        /* Save current payload for next iteration */
        memcpy(prev_payload, in_pkt->payload, PAYLOAD_LEN);
    }
    return 0;
}
