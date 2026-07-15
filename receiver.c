/*
 * Receiver — deduplicate and forward immediately
 *
 * With immediate duplication from the sender, the receiver's job is simple:
 *   1. Receive packets from the relay (port 47002)
 *   2. Deduplicate: only forward the first copy of each sequence number
 *   3. Forward to the harness player (port 47020) immediately
 *
 * The harness player checks arrival time against the deadline, so delivering
 * as early as possible is optimal. No jitter buffer or playout timer needed.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define PKT_LEN     (4 + PAYLOAD_LEN)
#define MAX_FRAMES  16000

static uint8_t delivered[MAX_FRAMES];

int main(void)
{
    memset(delivered, 0, sizeof(delivered));

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) {
        perror("bind 47002");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < PKT_LEN) continue;

        uint32_t seq;
        memcpy(&seq, buf, 4);
        seq = ntohl(seq);

        if (seq < MAX_FRAMES && !delivered[seq]) {
            delivered[seq] = 1;
            sendto(out_fd, buf, PKT_LEN, 0,
                   (struct sockaddr *)&player, sizeof(player));
        }
    }
    return 0;
}
