# Design Notes

The sender duplicates every packet immediately — two back-to-back `sendto`
calls for each frame received from the harness. The relay draws independent
drop and delay decisions for each copy, so a frame is only lost if both
copies are dropped or both arrive after the deadline. With 2% independent
loss this gives P(miss) ≈ 0.04% per frame.

The 2.0× bandwidth cap limits us to ~1426 duplicates out of 1500 frames
(at 164 bytes each, 2926 total packets). The last 74 frames are sent once.

The receiver is stateless: it deduplicates by sequence number and forwards
each frame to the player the instant it arrives. No jitter buffer, no
playout timer — the harness player only checks whether a frame arrived
before its deadline, so delivering early is optimal.

Recommended grading delay: **40 ms** for mild profiles (relay delay ≤ 40ms),
**82 ms** for moderate profiles (relay delay ≤ 80ms). The delay should be
set to the maximum relay delay plus 2ms of processing margin.

**What breaks it**: burst losses where the relay drops many consecutive
packets — since both copies are sent back-to-back, a sustained burst
can kill both. Profiles with loss rates above ~7% will push the
unprotected tail frames over 1%. Delay spikes beyond the configured
delay_ms cause misses regardless of duplication.
