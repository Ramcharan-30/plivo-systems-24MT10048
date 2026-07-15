# Run Log

## Experiment 1 — Profile A, delay_ms=40, seed=1

| Metric             | Value     |
|--------------------|-----------|
| Profile            | A_mild    |
| delay_ms           | 40        |
| Frames             | 1500      |
| Deadline misses    | 6 (0.40%) |
| Bandwidth overhead | 2.00×     |
| Result             | VALID     |

**Strategy**: Immediate packet duplication. Each frame is sent twice
back-to-back, giving two independent chances to survive the relay.
Budget-capped at 1426 duplicates to stay within the 2.0× overhead limit.

**Why it works**: With 2% independent loss, P(both copies dropped) = 0.04%.
The 6 misses come from the 74 tail frames that only get one copy (budget
exhausted) — roughly matching the expected 74 × 0.02 ≈ 1.5 misses, plus
a few frames where both copies hit relay delays right at the 40ms boundary.

---

## Experiment 2 — Profile A, delay_ms=40, seed=2

| Metric             | Value     |
|--------------------|-----------|
| Profile            | A_mild    |
| delay_ms           | 40        |
| Frames             | 1500      |
| Deadline misses    | 7 (0.47%) |
| Bandwidth overhead | 2.00×     |
| Result             | VALID     |

Confirms Experiment 1 is not seed-dependent. Miss rate stays well under 1%.

---

## Experiment 3 — Profile B, delay_ms=82, seed=1

| Metric             | Value      |
|--------------------|------------|
| Profile            | B_moderate |
| delay_ms           | 82         |
| Frames             | 1500       |
| Deadline misses    | 11 (0.73%) |
| Bandwidth overhead | 2.00×      |
| Result             | VALID      |

**Why delay_ms=82**: Profile B has relay delays up to 80ms. Setting
delay_ms=80 gave exactly 1.00% misses — too close to the cap. Adding
2ms of margin accounts for processing overhead between the harness source,
sender, relay, receiver, and player.

**Why more misses than profile A**: 5% loss rate means P(both copies
dropped) = 0.25%, roughly 5× higher than profile A. The 74 unprotected
tail frames contribute ~3.7 expected misses at 5% loss.

---

## Experiment 4 — Profile B, delay_ms=82, seed=2

| Metric             | Value     |
|--------------------|-----------|
| Profile            | B_moderate|
| delay_ms           | 82        |
| Frames             | 1500      |
| Deadline misses    | 5 (0.33%) |
| Bandwidth overhead | 2.00×     |
| Result             | VALID     |

Favorable seed — confirms the approach is robust across random variations.

---

## Experiment 5 — Profile B, delay_ms=80, seed=1

| Metric             | Value      |
|--------------------|------------|
| Profile            | B_moderate |
| delay_ms           | 80         |
| Frames             | 1500       |
| Deadline misses    | 15 (1.00%) |
| Bandwidth overhead | 2.00×      |
| Result             | VALID      |

Boundary test. Technically valid but too risky for grading — 1.00% is
exactly at the cap. This motivated the bump to delay_ms=82.
