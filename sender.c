/*
 * Sender — Ultra-Low Latency Kernel Optimization
 *
 * Implements N-1 redundancy using strictly packed structures.
 * Leverages explicit endianness macros (htonl/ntohl) for guaranteed
 * network byte-order safety across host architectures.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160

/* Strictly packed to eliminate compiler-inserted structure padding */
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

int main(void)
{
    const char *dur_str = getenv("DURATION_S");
    double duration = dur_str ? atof(dur_str) : 30.0;
    int n_frames = (int)(duration * 1000.0 / 20.0);
    
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
            /* Ensure correct byte order natively */
            struct PktSingle out_pkt;
            out_pkt.seq = htonl(host_seq);
            memcpy(out_pkt.payload, in_pkt->payload, PAYLOAD_LEN);
            
            sendto(out_fd, &out_pkt, sizeof(struct PktSingle), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
        } else {
            struct PktDual dual;
            dual.seq = htonl(host_seq);
            memcpy(dual.payload_curr, in_pkt->payload, PAYLOAD_LEN);
            memcpy(dual.payload_prev, prev_payload, PAYLOAD_LEN);
            
            sendto(out_fd, &dual, sizeof(struct PktDual), 0,
                   (struct sockaddr *)&relay, sizeof(relay));
            dual_sent++;
        }

        memcpy(prev_payload, in_pkt->payload, PAYLOAD_LEN);
    }
    return 0;
}
