# MicroPulse benchmark run

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
