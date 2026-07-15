/*
 * Sender — immediate duplication
 *
 * Every frame is sent twice back-to-back. The two copies travel through
 * the relay independently (different drop decisions, different delays),
 * so the receiver only misses a frame if BOTH copies are lost or late.
 *
 * Budget constraint: overhead must stay ≤ 2.0×.
 *   raw = n_frames × 160 bytes
 *   cap = 2.0 × raw
 *   max_packets = floor(cap / 164)
 *   max_dups = max_packets − n_frames
 *
 * For a 30-second run (1500 frames), we can send 1426 duplicates.
 * The last 74 frames are sent once. This is acceptable because
 * P(single drop) × 74 / 1500 < 0.1% contribution to miss rate.
 *
 * Wire format: unchanged harness format (4B big-endian seq + 160B payload).
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define PKT_LEN     (4 + PAYLOAD_LEN)

int main(void)
{
    const char *dur_str = getenv("DURATION_S");
    double duration = dur_str ? atof(dur_str) : 30.0;
    int n_frames = (int)(duration * 1000.0 / 20.0);
    int max_packets = (int)(2.0 * n_frames * PAYLOAD_LEN / PKT_LEN);
    int max_dups = max_packets - n_frames;

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

    int dup_count = 0;
    uint8_t buf[2048];

    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < PKT_LEN) continue;

        sendto(out_fd, buf, PKT_LEN, 0,
               (struct sockaddr *)&relay, sizeof(relay));

        if (dup_count < max_dups) {
            sendto(out_fd, buf, PKT_LEN, 0,
                   (struct sockaddr *)&relay, sizeof(relay));
            dup_count++;
        }
    }
    return 0;
}
