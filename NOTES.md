# Design Notes: Ultra-Low Latency C Architecture

The system has been completely refactored from basic UDP redundancy into a highly generalized, production-grade Linux networking infrastructure.

## Generalized Loss Protection (N-1 Redundancy)
To prevent catastrophic failure against hostile burst-drop network profiles, the `sender` implements temporally-spaced N-1 redundancy. Instead of duplicating packets immediately, it dynamically multiplexes Frame N and Frame N-1 into a single UDP datagram. Utilizing strict `__attribute__((packed))` wire formats, it transmits exactly 1462 dual-payload packets (324 bytes) and 38 single-payload packets (164 bytes) to perfectly maximize the mathematical 2.0x bandwidth limit (1.99x).

## Adaptive 95th-Percentile Jitter Buffer
The `receiver` is stateful and highly adaptive. Instead of anchoring to static timeouts, it maintains a sliding window of the last 50 network transit delays, calculating a live **95th percentile (p95)** transit baseline. This allows the internal playout loop to "breathe"—stretching the delay to dynamically absorb spikes, and shrinking when the network stabilizes, bounded strictly by the harness's absolute `DELAY_MS` to prevent artificial deadline misses.

## Vectorized Kernel I/O (`recvmmsg` & `epoll`)
The receiver's fast-path pipeline uses `epoll_wait` (zero-CPU wait) coupled with `recvmmsg` in non-blocking mode (`MSG_DONTWAIT`), pulling up to 32 datagrams (`VLEN 32`) from the kernel buffer per context switch. This dramatically reduces syscall overhead during severe network jitter bursts.

## Memory Safety & Monotonic Spin-Locks
- Zero dynamic memory allocations (`malloc`/`free`). The $O(1)$ ring buffer (`struct Frame ring[2048]`), `mmsghdr`, and jitter trackers are strictly initialized in static `BSS`.
- The ring buffer is explicitly `__attribute__((aligned(64)))` to eliminate L1 cache-line false sharing.
- Final playout dispatch targets are calculated using `clock_gettime(CLOCK_MONOTONIC)`, entering a hyper-precise `__asm__ volatile("pause" ::: "memory")` spin-lock for the final 50 microseconds before dispatch to bypass OS scheduling latency.
