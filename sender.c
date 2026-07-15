/*
 * Sender — XOR FEC (group-of-2 parity)
 *
 * For every pair of consecutive frames (2k, 2k+1), we transmit:
 *   1. The original frame 2k   (as soon as the harness delivers it)
 *   2. The original frame 2k+1 (as soon as the harness delivers it)
 *   3. An XOR parity packet    (sent right after frame 2k+1)
 *
 * Wire format (same 164-byte size as the harness format):
 *   [4 bytes] seq  — big-endian uint32
 *                     for data packets: frame index
 *                     for parity: first seq of the pair | 0x80000000
 *   [160 bytes] payload — frame data or XOR of the pair
 *
 * If any single packet in a pair is lost, the receiver can XOR the
 * surviving packet with the parity to recover the missing one.
 *
 * Bandwidth: 3 packets per 2 frames → 1.5× overhead (well under 2.0×).
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define PKT_LEN     (4 + PAYLOAD_LEN)
#define FEC_FLAG    0x80000000u

static void send_pkt(int fd, const struct sockaddr_in *dst,
                     uint32_t seq, const uint8_t *payload)
{
    uint8_t buf[PKT_LEN];
    uint32_t net_seq = htonl(seq);
    memcpy(buf, &net_seq, 4);
    memcpy(buf + 4, payload, PAYLOAD_LEN);
    sendto(fd, buf, PKT_LEN, 0, (struct sockaddr *)dst, sizeof(*dst));
}

int main(void)
{
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
    uint32_t prev_seq = 0;
    int have_prev = 0;

    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < PKT_LEN) continue;

        uint32_t seq;
        memcpy(&seq, buf, 4);
        seq = ntohl(seq);
        uint8_t *payload = buf + 4;

        /* always forward the original frame immediately */
        send_pkt(out_fd, &relay, seq, payload);

        if (have_prev) {
            /* send XOR parity for the pair (prev_seq, seq) */
            uint8_t xor_payload[PAYLOAD_LEN];
            for (int i = 0; i < PAYLOAD_LEN; i++)
                xor_payload[i] = prev_payload[i] ^ payload[i];

            uint32_t parity_seq = prev_seq | FEC_FLAG;
            send_pkt(out_fd, &relay, parity_seq, xor_payload);
            have_prev = 0;
        } else {
            memcpy(prev_payload, payload, PAYLOAD_LEN);
            prev_seq = seq;
            have_prev = 1;
        }
    }
    return 0;
}
