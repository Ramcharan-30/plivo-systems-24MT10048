/*
 * Receiver — jitter buffer + XOR FEC recovery
 *
 * Two threads:
 *   recv_thread  — reads packets from relay (port 47002), stores originals in
 *                  a frame buffer, and attempts XOR recovery when a parity
 *                  packet arrives and one of the pair is missing.
 *   main thread  — playout loop that delivers frames to the harness player
 *                  (port 47020) at exactly the right time.
 *
 * Wire format matches the sender: 4-byte big-endian seq + 160-byte payload.
 * Bit 31 of seq distinguishes parity packets from data packets.
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define PKT_LEN     (4 + PAYLOAD_LEN)
#define FEC_FLAG    0x80000000u
#define MAX_FRAMES  16000

typedef struct {
    uint8_t payload[PAYLOAD_LEN];
    int     present;
} frame_slot;

static frame_slot  frames[MAX_FRAMES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* parity buffer: store parity packets until the pair can be recovered */
typedef struct {
    uint8_t payload[PAYLOAD_LEN];
    uint32_t base_seq; /* first seq of the pair */
    int     present;
} parity_slot;

static parity_slot parities[MAX_FRAMES / 2 + 1];

static void store_frame(uint32_t seq, const uint8_t *payload)
{
    if (seq >= MAX_FRAMES) return;
    if (frames[seq].present) return; /* dedup */
    memcpy(frames[seq].payload, payload, PAYLOAD_LEN);
    frames[seq].present = 1;
}

static void try_fec_recover(uint32_t base_seq)
{
    if (base_seq + 1 >= MAX_FRAMES) return;
    uint32_t pidx = base_seq / 2;
    if (pidx >= MAX_FRAMES / 2 + 1) return;
    if (!parities[pidx].present) return;

    int have_a = frames[base_seq].present;
    int have_b = frames[base_seq + 1].present;

    if (have_a && have_b) return; /* nothing to recover */
    if (!have_a && !have_b) return; /* can't recover both */

    uint32_t missing = have_a ? base_seq + 1 : base_seq;
    uint32_t present_seq = have_a ? base_seq : base_seq + 1;

    uint8_t recovered[PAYLOAD_LEN];
    for (int i = 0; i < PAYLOAD_LEN; i++)
        recovered[i] = parities[pidx].payload[i] ^ frames[present_seq].payload[i];

    store_frame(missing, recovered);
}

struct recv_args {
    int in_fd;
};

static void *recv_thread(void *arg)
{
    struct recv_args *ra = (struct recv_args *)arg;
    uint8_t buf[2048];

    for (;;) {
        ssize_t n = recvfrom(ra->in_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < PKT_LEN) continue;

        uint32_t raw_seq;
        memcpy(&raw_seq, buf, 4);
        raw_seq = ntohl(raw_seq);
        uint8_t *payload = buf + 4;

        pthread_mutex_lock(&lock);

        if (raw_seq & FEC_FLAG) {
            uint32_t base_seq = raw_seq & ~FEC_FLAG;
            uint32_t pidx = base_seq / 2;
            if (pidx < MAX_FRAMES / 2 + 1) {
                memcpy(parities[pidx].payload, payload, PAYLOAD_LEN);
                parities[pidx].base_seq = base_seq;
                parities[pidx].present = 1;
            }
            try_fec_recover(base_seq);
        } else {
            store_frame(raw_seq, payload);
            /* try FEC recovery for the pair this frame belongs to */
            uint32_t base = (raw_seq & 1) ? raw_seq - 1 : raw_seq;
            try_fec_recover(base);
        }

        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static double gettime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_until(double t)
{
    double now = gettime();
    if (t <= now) return;
    struct timespec ts;
    double wait = t - now;
    ts.tv_sec = (time_t)wait;
    ts.tv_nsec = (long)((wait - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

int main(void)
{
    const char *t0_str = getenv("T0");
    const char *dur_str = getenv("DURATION_S");
    const char *delay_str = getenv("DELAY_MS");
    if (!t0_str || !dur_str || !delay_str) {
        fprintf(stderr, "missing T0/DURATION_S/DELAY_MS env vars\n");
        return 1;
    }

    double t0 = atof(t0_str);
    double duration = atof(dur_str);
    double delay_ms = atof(delay_str);
    int n_frames = (int)(duration * 1000.0 / 20.0);

    memset(frames, 0, sizeof(frames));
    memset(parities, 0, sizeof(parities));

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

    struct recv_args ra = { .in_fd = in_fd };
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, &ra);

    /* playout loop: deliver each frame just before its deadline */
    for (int i = 0; i < n_frames; i++) {
        double deadline = t0 + delay_ms / 1000.0 + i * 0.020;
        /* wake up 1ms before deadline to account for scheduling jitter */
        sleep_until(deadline - 0.001);

        pthread_mutex_lock(&lock);
        int have = (i < MAX_FRAMES) && frames[i].present;
        uint8_t payload[PAYLOAD_LEN];
        if (have)
            memcpy(payload, frames[i].payload, PAYLOAD_LEN);
        pthread_mutex_unlock(&lock);

        if (have) {
            uint8_t pkt[PKT_LEN];
            uint32_t net_seq = htonl((uint32_t)i);
            memcpy(pkt, &net_seq, 4);
            memcpy(pkt + 4, payload, PAYLOAD_LEN);
            sendto(out_fd, pkt, PKT_LEN, 0,
                   (struct sockaddr *)&player, sizeof(player));
        }
    }

    return 0;
}
