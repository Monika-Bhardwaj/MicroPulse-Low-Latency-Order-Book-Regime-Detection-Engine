# MicroPulse — latency / throughput benchmark report

**Read first.** Everything below was measured on a **shared 1-vCPU virtual machine** (Intel Xeon @ 2.8 GHz as reported by
`/proc/cpuinfo`, 4 GB RAM, g++ 13 `-O3`) on **synthetic data**. It is "commodity hardware" in the weak sense: no CPU pinning,
no isolated core, noisy neighbours. Consequences: (1) `max` latencies (hundreds of µs to ms) are VM scheduling / page-fault noise and
should be ignored; p50–p99 are repeatable, p99.9 less so; (2) the two-thread pipeline cannot be latency-benchmarked meaningfully on one
vCPU, so queue behaviour is studied with a virtual-time queue driven by *measured* service times (section 5); (3) absolute numbers will
differ on dedicated hardware — I observed the deep-stuffing penalty in section 4 vary by roughly an order of magnitude between two runs
of the same experiment on different VM states during development (the mechanism and direction were identical, the magnitude was not), so
treat section 4's *ratios* as hardware-dependent. Reproduce with `./build/bench_main all`.

Methodology: per-message service latency is `rdtscp` before/after `Engine::on_delta` / `on_frame` (timer overhead 13 ns, not subtracted);
throughput comes from a separate bulk pass without per-message timers (median of 3); snapshot/resync handling is excluded from service
latency; a "message" carries ~2 level updates. Event timestamps come from the stream; the local monotonic clock is used for latency only.

## Headline numbers (default configuration: capped book 256 levels/side)

| metric | value |
|---|---|
| core throughput, book + features | **5.95 M msgs/s** (≈ 12 M level updates/s); 5.29 M msgs/s with CUSUM ×2 + BOCPD attached |
| core per-message latency, no detectors | p50 **169 ns**, p90 281 ns, p99 **446 ns**, p99.9 733 ns |
| core per-message latency, 3 detectors | p50 174 ns, p99 421 ns, **p99.9 ≈ 9.9 µs** (the BOCPD update at each 250 ms bar close, ~1 in 600 messages) |
| JSON parse only (Binance depthUpdate) | 343 ns/msg, **2.91 M msgs/s, 434 MB/s** |
| frame → parse → book → features | p50 **499 ns**, p99 1.13 µs |
| incremental vs naive recompute (book + features) | **1.6× / 11.9× / 105×** at 0.1 s / 1 s / 10 s windows (≈ 87 / 952 / 9 370 samples) |
| book memory | full depth ≈ 128 KB at ~3 100 levels/side; capped-256 **8 KB** (hard bound); top-32 1 KB |
| deep quote stuffing, stuffing messages | p50 **717 ns → 91 ns**, p99.9 18.2 µs → 0.44 µs (unbounded book → capped-256) |

## Interpretation

**1. Parser.** A hand-written, allocation-free scanner; no floating point (decimal strings → int64 at 1e-8). On this stream (149 bytes, 2 levels
per message) parsing is ~2/3 of the end-to-end frame cost; the book + features are ~160 ns. A Binance `@depth@100ms` stream delivers ~10 messages/s
per symbol, so one core has several orders of magnitude of headroom for this parser; the high rates are for stress.

**2. Core latency.** Steady-state cost is nearly independent of depth mode (p50 158–174 ns): real flow hits the touch, where the sorted-vector
book needs a short galloping search and a tiny memmove. The detector cost is invisible at p99 and appears only at p99.9 (~10 µs, the BOCPD
log-space run-length update over ≤ 201 hypotheses, once per bar). If that tail matters it can be moved off the hot thread or `rmax` reduced;
it is not hidden here.

**3. Incremental vs naive.** The speedup is *not* a constant: naive recompute is O(window), incremental is O(1), so the ratio grows with the number of
samples in the window (1.6× at ~90 samples, 105× at ~9 400). The reported speedups are for the whole book + features path, so they understate the
pure window-update ratio. Both implementations are checked equal in `test_incremental_equals_naive` (OFI and counts exact, realised-vol relative
error < 1e-9).

**4. Quote stuffing (before / after hardening).** Attack: 1 s bursts at 100 k msg/s, six per minute. *Deep* stuffing places and cancels tiny orders at a
price worse than every resting level, which forces a front-insert and front-erase — an O(n) memmove — in a flat sorted array. With an unbounded book that
costs p50 717 ns / p99.9 18 µs per stuffing message; with the depth cap the update is rejected in O(1) (p50 91 ns). *Touch* stuffing (quantity flicker at the best price)
is not made worse by an unbounded book and is **not improved by hardening** — that is an inherent cost of updating the touch (~100 ns). Background
(legitimate) messages are essentially unaffected in both configurations (p50 172–176 ns); p99.9 ≈ 9 µs there is the bar-close detector update, not the attack.
The honest summary: on this hardware the unhardened book tolerates a 100 k msg/s deep-stuffing burst (≈ 7–10 % of a core); hardening buys ~2.6× capacity
under that attack (section 5: 1.0 M vs 2.56 M msg/s) rather than rescuing a collapsing system. The failure point of the unhardened book is a stuffing rate of
roughly 1 M msg/s on this machine, which I did not test.

**5. Burst load and backpressure.** Virtual-time single-server queue, open-loop arrivals at event-time pace, latency from the intended arrival time
(no coordinated omission), 1024-slot ring, 50 ms modelled resync latency. *Legit burst*: 20 ms bursts at ~3 M msg/s (≈ 1.8× the measured 1.69 M msg/s
capacity of the full pipeline incl. parse and detectors), 12 per minute. **Block** loses nothing and keeps the book valid, at the price of queueing: p99 21–28 ms,
max ≈ 35 ms, producer stalled ~1.1 s of 120 s. **Drop newest** keeps latency < 1.3 ms for the frames it accepts but discards 12–20 % of messages, produces
34 sequence gaps → 34 resyncs and leaves the book invalid for 1.4 % of the time (each resync modelled as 50 ms). Neither policy is "right": Block preserves data
integrity and is correct if downstream can tolerate ≤ ~35 ms staleness after a burst; Drop bounds latency but every drop is a flagged resync. (Latency of
dropped frames is not defined, so Drop's percentiles are survivorship-biased — read them with the drop and invalid-book columns.) The capped engine drops fewer
frames than the full-depth one (11.9 % vs 19.9 %) because it is faster per message under bursts. *Attack scenario*: at real-time pace the 100 k msg/s deep-stuffing bursts
never saturate either engine on this hardware (max ring occupancy 79–311 of 1024), so queue-level results are unremarkable and I do not claim a hardening win there beyond
the service-time numbers above.

**6. Memory and the default.** The book itself is small in absolute terms (128 KB at ~3 100 levels/side); the rolling OFI ring (16 384 × 24 B = 384 KB)
dominates. The cap is therefore chosen for **bounded worst-case latency and bounded memory under hostile input, not for the average memory saving**. Defaults:
`max_levels = 256` — same steady-state latency as full depth, a hard 8 KB book bound, and zero refill snapshots over 400 s of synthetic flow at this volatility.
`TOP-32` is the extreme point: 1 KB, fastest (6.9 M msgs/s) but it needed 150–300 refill snapshots per 400 s (one every ~1.3–2.6 s), which against a REST-based
snapshot is an unacceptable request rate — the explicit latency/memory/operational trade-off, and why 256 is the default. (Refill frequency depends on volatility and tick size; re-measure on real data.)

**7. Threaded pipeline.** A smoke test only (1 vCPU): Block delivered all 146 868 frames; Drop discarded 102 795, detected 44 gaps and resynced 44 times with every
accounted-for frame processed or dropped. Throughput there is dominated by context switching.

## What is not measured

* Real Binance traffic, exchange→host network delay, and the exchange/host clock offset (not measurable here; see README "Clock skew").
* Real resync latency and REST rate limits (modelled as instantaneous in sections 2–4, 50 ms in section 5).
* Dedicated-core, pinned, huge-page performance; NUMA effects; multi-symbol multiplexing.
* Latency of the two-thread `Pipeline` itself.

---

# Raw results (generated by `bench_main all`)


Hardware: Intel(R) Xeon(R) Processor @ 2.80GHz, 1 logical CPU(s) visible; rdtscp ticks at 2.800 GHz. Build: -O3 -std=c++17.

## 1. Parser throughput (Binance depthUpdate JSON, hand-written parser)

| messages | avg bytes/msg | avg levels/msg | parse ns/msg | msgs/s | MB/s |
|---|---|---|---|---|---|
| 244956 | 149 | 2.00 | 343 | 2.91 M | 434 |

Frame -> parse -> book -> features (CAPPED 256, no detectors), per-message service latency: p50 499 ns, p99 1126 ns, p99.9 5251 ns, max 110811 ns (n=244956, stat resyncs=0)

## 2. Core latency / throughput (parsed deltas -> book -> features [-> detectors])

Stream: 980099 messages over 400 s event time (synthetic, seed 1), 1.99 level updates/msg. Latencies in ns, per message.

| mode | detectors | throughput (msgs/s) | p50 | p90 | p99 | p99.9 | max | notes |
|---|---|---|---|---|---|---|---|---|
| FULL depth (unbounded) | none | 5.53 M | 158 | 256 | 384 | 649 | 61212 | end levels 3190+3207, refills 0, resyncs 0 |
| FULL depth (unbounded) | CUSUM x2 + BOCPD | 5.26 M | 174 | 282 | 429 | 9968 | 1247434 | end levels 3190+3207, refills 0, resyncs 0 |
| CAPPED 256/side | none | 5.95 M | 169 | 281 | 446 | 733 | 192254 | end levels 114+134, refills 0, resyncs 0 |
| CAPPED 256/side | CUSUM x2 + BOCPD | 5.29 M | 174 | 281 | 421 | 9934 | 400638 | end levels 114+134, refills 0, resyncs 0 |
| TOP-32/side | none | 6.90 M | 174 | 288 | 460 | 897 | 111780 | end levels 22+28, refills 301, resyncs 0 |
| TOP-32/side | CUSUM x2 + BOCPD | 4.95 M | 156 | 248 | 401 | 9330 | 409930 | end levels 22+28, refills 301, resyncs 0 |

Timer overhead (back-to-back rdtscp, p50): 13 ns (included in all latencies above).

## 3. Incremental vs naive recompute-from-scratch features

Same book, same stream; only the rolling-window implementation differs (Welford add/remove + exact integer sums vs two-pass recompute over the window every time it changes). No detectors, CAPPED 256. Throughput = messages/s through book + features.

| window | touch-change samples in window (avg) | incremental msgs/s | naive msgs/s | **speedup** | incremental ns/msg | naive ns/msg |
|---|---|---|---|---|---|---|
| 0.1 s | ~87 | 4.74 M | 3.021 M | **1.6x** | 211 | 331 |
| 1.0 s | ~952 | 6.15 M | 0.518 M | **11.9x** | 163 | 1932 |
| 10.0 s | ~9370 | 6.41 M | 0.061 M | **105.5x** | 156 | 16465 |

## 4. Quote-stuffing stress on the hot path (before / after hardening)

Attack streams: 1.0 s bursts at 100,000 msg/s (6 per minute) of add/cancel pairs.  DEEP = new worst-price level on a random side (forces a front-insert/erase in a flat sorted array); TOUCH = quantity flicker at the best price.  Background traffic: 2,000 msg/s regime-free churn.  Per-message service latency (ns), split by message class.

| attack | engine | class | n | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| DEEP | BEFORE: FULL depth, no cap | stuffing msgs | 1299082 | 717 | 1136 | 1493 | 18209 | 939074 |
| DEEP | BEFORE: FULL depth, no cap | background msgs | 236623 | 176 | 299 | 498 | 9652 | 106815 |
| DEEP | AFTER: CAPPED 256 | stuffing msgs | 1299082 | 91 | 116 | 217 | 441 | 449635 |
| DEEP | AFTER: CAPPED 256 | background msgs | 236623 | 172 | 289 | 484 | 8777 | 309242 |
| DEEP | AFTER: TOP-32 | stuffing msgs | 1299082 | 80 | 94 | 140 | 306 | 592058 |
| DEEP | AFTER: TOP-32 | background msgs | 236623 | 153 | 240 | 372 | 8525 | 59526 |
| TOUCH | BEFORE: FULL depth, no cap | stuffing msgs | 1297963 | 97 | 132 | 204 | 445 | 128660 |
| TOUCH | BEFORE: FULL depth, no cap | background msgs | 236364 | 171 | 290 | 618 | 9030 | 99324 |
| TOUCH | AFTER: CAPPED 256 | stuffing msgs | 1297963 | 99 | 147 | 216 | 501 | 358517 |
| TOUCH | AFTER: CAPPED 256 | background msgs | 236364 | 173 | 289 | 608 | 9283 | 803420 |
| TOUCH | AFTER: TOP-32 | stuffing msgs | 1297963 | 99 | 142 | 224 | 463 | 405970 |
| TOUCH | AFTER: TOP-32 | background msgs | 236364 | 161 | 269 | 590 | 8877 | 265415 |

## 5. Burst load: bounded queue, backpressure vs drop (virtual-time queue, measured service times)

Mean service time (JSON parse + book + features + 3 detectors): CAPPED-256 on the legit stream 591 ns/msg (1.69 M msg/s capacity); on the attack stream FULL-depth 1002 ns/msg (0.998 M msg/s) vs CAPPED-256 390 ns/msg (2.56 M msg/s).
Open-loop arrivals at event-time pace; ring = 1024 slots; resync latency modelled as 50 ms (async REST snapshot; frames arriving meanwhile are dropped as 'unsynced'). Latency is measured from the INTENDED arrival time.

### LEGIT burst: 20 ms bursts at ~3 M msg/s, 12 per minute, no manipulation  (2233502 messages over 120 s)

| engine | policy | dropped | gaps -> resyncs | time with invalid book | max ring occupancy | e2e p50 | e2e p99 | e2e p99.9 | e2e max | e2e p99, background msgs only | producer stalled |
|---|---|---|---|---|---|---|---|---|---|---|---|
| FULL depth (no cap) | Block | 0 (0.0%) | 0 -> 0 | 0.0% | 1024 | 5.6 ms | 28.4 ms | 33.6 ms | 35.1 ms | 28.4 ms | 1113.5 ms |
| FULL depth (no cap) | Drop newest | 443383 (19.9%) | 34 -> 34 | 1.4% | 1024 | 380.2 us | 1.0 ms | 1.3 ms | 1.6 ms | 1.0 ms | 0.0 ms |
| CAPPED 256 | Block | 0 (0.0%) | 0 -> 0 | 0.0% | 1024 | 5.6 ms | 21.1 ms | 25.3 ms | 25.8 ms | 21.1 ms | 1091.6 ms |
| CAPPED 256 | Drop newest | 266500 (11.9%) | 34 -> 34 | 1.4% | 1024 | 374.0 us | 785.9 us | 1.2 ms | 2.2 ms | 785.9 us | 0.0 ms |

### ATTACK: 1 s deep quote-stuffing bursts at 100k msg/s, 6 per minute, over 2k msg/s background  (2036404 messages over 120 s)

| engine | policy | dropped | gaps -> resyncs | time with invalid book | max ring occupancy | e2e p50 | e2e p99 | e2e p99.9 | e2e max | e2e p99, background msgs only | producer stalled |
|---|---|---|---|---|---|---|---|---|---|---|---|
| FULL depth (no cap) | Block | 0 (0.0%) | 0 -> 0 | 0.0% | 79 | 1.0 us | 3.6 us | 67.3 us | 986.5 us | 1.7 us | 0.0 ms |
| FULL depth (no cap) | Drop newest | 0 (0.0%) | 0 -> 0 | 0.0% | 311 | 1.0 us | 5.7 us | 108.8 us | 3.2 ms | 1.8 us | 0.0 ms |
| CAPPED 256 | Block | 0 (0.0%) | 0 -> 0 | 0.0% | 216 | 0.3 us | 1.0 us | 33.2 us | 2.1 ms | 1.3 us | 0.0 ms |
| CAPPED 256 | Drop newest | 0 (0.0%) | 0 -> 0 | 0.0% | 218 | 0.3 us | 1.2 us | 29.9 us | 2.0 ms | 1.5 us | 0.0 ms |

## 6. Memory footprint: full depth vs top-of-book-only

Container bytes are exact (vector capacity x 16 B per level); "worst-case bound" is what a hostile feed can force. RSS is not reported: page granularity and allocator reuse make it meaningless at this scale.

| mode | valid levels at end (bid+ask) | book bytes (steady) | book bytes (worst-case bound) | rolling-window ring | BOCPD arrays | p50 ns | p99 ns | refills |
|---|---|---|---|---|---|---|---|---|
| FULL depth (unbounded) | 3075 + 3109 | 128.0 KB | unbounded | 384 KB | 12.6 KB | 173 | 433 | 0 |
| CAPPED 256/side | 117 + 147 | 8.0 KB | 8.0 KB | 384 KB | 12.6 KB | 173 | 434 | 0 |
| TOP-32/side | 18 + 31 | 1.0 KB | 1.0 KB | 384 KB | 12.6 KB | 185 | 532 | 153 |

## 7. Threaded SPSC pipeline smoke test (1 vCPU: throughput only, latency not meaningful)

| policy | submitted | processed | dropped | gaps -> resyncs | blocked spins | wall msgs/s |
|---|---|---|---|---|---|---|
| Block | 146868 | 146868 | 0 | 0 -> 0 | 150 | 1.44 M |
| Drop newest | 146868 | 44073 | 102795 | 44 -> 44 | 0 | 2.47 M |
