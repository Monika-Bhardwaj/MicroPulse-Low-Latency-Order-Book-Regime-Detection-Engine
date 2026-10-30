# Calibration run (auto-generated)

8 calibration seeds and 8 disjoint held-out seeds per class, 1800 s each; 73 labeled changes in calibration, 71 in held-out. FP budget 1.0/hour. Runtime 550 s.

## Calibration sweep (vol channel, calibration seeds, pooled clean+hostile null)

### CUSUM (k = 0.5)

| h | null FP / hour | alarms / hours | recall (changes detected within 45 s) | mean delay (s) |
|---|---|---|---|---|
| 4 | 100.01 | 760 / 7.6 | 0.96 | 8.9 |
| 5 | 73.17 | 556 / 7.6 | 0.92 | 8.6 |
| 6 | 50.27 | 382 / 7.6 | 0.95 | 10.0 |
| 7 | 34.35 | 261 / 7.6 | 0.90 | 7.9 |
| 8 | 24.35 | 185 / 7.6 | 0.90 | 5.2 |
| 10 | 11.05 | 84 / 7.6 | 0.93 | 5.9 |
| 12 | 4.87 | 37 / 7.6 | 0.88 | 7.0 |
| 14 | 1.71 | 13 / 7.6 | 0.85 | 6.9 |
| 17 | 0.00 | 0 / 7.6 | 0.84 | 9.4 |
| 20 | 0.00 | 0 / 7.6 | 0.78 | 10.3 |
| 25 | 0.00 | 0 / 7.6 | 0.77 | 13.6 |

### BOCPD (best 12 settings by recall among those meeting the budget, then 3 most sensitive that do not)

| hazard lambda | k_short | clip (sd) | threshold | null FP / hour | recall | mean delay (s) | meets budget |
|---|---|---|---|---|---|---|---|
| 1000 | 80 | 2.5 | 0.999 | 0.92 | 0.56 | 5.3 | yes |
| 3000 | 80 | 2.5 | 0.999 | 0.53 | 0.56 | 5.6 | yes |
| 3000 | 80 | 3.0 | 0.999 | 0.39 | 0.56 | 6.2 | yes |
| 1000 | 80 | 2.5 | 0.9995 | 0.66 | 0.55 | 5.2 | yes |
| 3000 | 80 | 2.5 | 0.9995 | 0.39 | 0.55 | 5.8 | yes |
| 3000 | 80 | 3.0 | 0.9995 | 0.39 | 0.49 | 5.5 | yes |
| 3000 | 40 | 2.5 | 0.995 | 0.39 | 0.38 | 3.5 | yes |
| 3000 | 40 | 3.0 | 0.995 | 0.92 | 0.36 | 3.3 | yes |
| 1000 | 40 | 2.5 | 0.999 | 0.26 | 0.34 | 3.3 | yes |
| 1000 | 20 | 2.5 | 0.97 | 0.53 | 0.33 | 2.5 | yes |
| 1000 | 20 | 3.0 | 0.97 | 0.79 | 0.33 | 2.5 | yes |
| 1000 | 40 | 3.0 | 0.999 | 0.26 | 0.33 | 3.2 | yes |
| 1000 | 80 | 2.5 | 0.9 | 20.79 | 0.85 | 5.3 | no |
| 1000 | 80 | 3.0 | 0.9 | 22.50 | 0.85 | 5.5 | no |
| 3000 | 80 | 2.5 | 0.9 | 12.37 | 0.85 | 5.7 | no |

Chosen operating points: CUSUM h = 17; BOCPD (lambda, k_short, clip, threshold) = (3000.0, 80, 3.0, 0.999).

## Held-out evaluation (disjoint seeds, thresholds frozen)

| detector | null set | null exposure (h) | false alarms | **FP / hour** (95% CI) | changes | recall | mean delay (s) | p90 delay (s) |
|---|---|---|---|---|---|---|---|---|
| cusum | clean | 3.8 | 0 | **0.00** (0.00-0.97) | 71 | 0.94 | 10.4 | 22.3 |
| cusum | hostile | 3.8 | 0 | **0.00** (0.00-0.97) | 71 | 0.94 | 10.4 | 22.3 |
| bocpd | clean | 3.8 | 5 | **1.32** (0.43-3.07) | 71 | 0.63 | 6.0 | 11.4 |
| bocpd | hostile | 3.8 | 3 | **0.79** (0.16-2.31) | 71 | 0.63 | 6.0 | 11.4 |

False alarms inside the labeled-change runs (outside every 45 s detection window): cusum: 0 in 2.9 h = 0.00/h (CI 0.00-1.27); bocpd: 3 in 2.9 h = 1.03/h (CI 0.21-3.01)

### Recall by shift magnitude (held-out)

| detector | shift (sigma ratio) | n | recall | mean delay (s) |
|---|---|---|---|---|
| cusum | down x2 | 26 | 0.96 | 12.4 |
| cusum | down x4 | 11 | 1.00 | 3.0 |
| cusum | up x2 | 21 | 0.86 | 15.5 |
| cusum | up x4 | 13 | 1.00 | 5.6 |
| bocpd | down x2 | 26 | 0.50 | 7.4 |
| bocpd | down x4 | 11 | 1.00 | 2.6 |
| bocpd | up x2 | 21 | 0.38 | 9.9 |
| bocpd | up x4 | 13 | 1.00 | 4.9 |

## Manipulation robustness (held-out seeds; thresholds calibrated on the CLEAN null only)

### Detector-input distortion (detector-independent, high power)

For every spoof episode (or, for the no-manipulation / stuffing scenarios, a control window of the same length at random times) we take the largest robust z-score (|x - median| / 1.4826 MAD) of the detector input over the window. Reported: share of windows with max z > 4, > 6, and the median / 95th percentile of the window maximum.

| scenario | statistic | windows | P(max z > 4) | P(max z > 6) | median max z | p95 max z |
|---|---|---|---|---|---|---|
| no manipulation (control windows) | VOL (grid RV) | 2880 | 0.019 | 0.000 | 2.52 | 3.06 |
| no manipulation (control windows) | FLOW naive (raw OFI sum, no winsorising) | 2880 | 0.045 | 0.005 | 1.83 | 3.87 |
| no manipulation (control windows) | FLOW hardened (winsorised OFI, t-stat) | 2880 | 0.019 | 0.000 | 1.86 | 3.44 |
| spoof AT touch (50x, 1 s, 12/min) | VOL (grid RV) | 2788 | 0.018 | 0.001 | 2.52 | 2.84 |
| spoof AT touch (50x, 1 s, 12/min) | FLOW naive (raw OFI sum, no winsorising) | 2788 | 0.161 | 0.107 | 2.04 | 7.21 |
| spoof AT touch (50x, 1 s, 12/min) | FLOW hardened (winsorised OFI, t-stat) | 2788 | 0.019 | 0.000 | 1.87 | 3.50 |
| spoof 3 ticks behind touch | VOL (grid RV) | 2788 | 0.020 | 0.000 | 2.52 | 2.96 |
| spoof 3 ticks behind touch | FLOW naive (raw OFI sum, no winsorising) | 2788 | 0.051 | 0.007 | 1.87 | 4.01 |
| spoof 3 ticks behind touch | FLOW hardened (winsorised OFI, t-stat) | 2788 | 0.019 | 0.000 | 1.84 | 3.41 |
| layered spoof (3 layers) at touch | VOL (grid RV) | 2788 | 0.014 | 0.001 | 2.52 | 2.92 |
| layered spoof (3 layers) at touch | FLOW naive (raw OFI sum, no winsorising) | 2788 | 0.226 | 0.150 | 2.11 | 7.62 |
| layered spoof (3 layers) at touch | FLOW hardened (winsorised OFI, t-stat) | 2788 | 0.021 | 0.000 | 1.89 | 3.52 |
| quote stuffing, touch flicker (50k/s) (control windows) | VOL (grid RV) | 2880 | 0.014 | 0.000 | 2.52 | 3.06 |
| quote stuffing, touch flicker (50k/s) (control windows) | FLOW naive (raw OFI sum, no winsorising) | 2880 | 0.057 | 0.007 | 1.86 | 4.14 |
| quote stuffing, touch flicker (50k/s) (control windows) | FLOW hardened (winsorised OFI, t-stat) | 2880 | 0.017 | 0.000 | 1.83 | 3.40 |
| quote stuffing, deep (50k/s) (control windows) | VOL (grid RV) | 2880 | 0.021 | 0.002 | 2.52 | 3.07 |
| quote stuffing, deep (50k/s) (control windows) | FLOW naive (raw OFI sum, no winsorising) | 2880 | 0.047 | 0.006 | 1.88 | 3.95 |
| quote stuffing, deep (50k/s) (control windows) | FLOW hardened (winsorised OFI, t-stat) | 2880 | 0.020 | 0.001 | 1.87 | 3.48 |

### Alarms

Each cell = false alarms per hour (95% Poisson CI) in 8 x 1800 s runs; `in-window` = alarms that landed inside a spoof episode (+5 s). Thresholds per statistic/detector were set to <= 1 FP/hour on clean calibration data.


### CUSUM

| scenario | VOL (grid RV) | FLOW naive (raw OFI sum, no winsorising) | FLOW hardened (winsorised OFI, t-stat) |
|---|---|---|---|
| no manipulation | 0.5 (0.06-1.90); in-window 0/2 | 0.8 (0.16-2.31); in-window 0/3 | 0.5 (0.06-1.90); in-window 0/2 |
| spoof AT touch (50x, 1 s, 12/min) | 0.0 (0.00-0.97); in-window 0/0 | 2.1 (0.91-4.15); in-window 7/8 | 0.3 (0.01-1.47); in-window 1/1 |
| spoof 3 ticks behind touch | 0.0 (0.00-0.97); in-window 0/0 | 1.1 (0.29-2.70); in-window 4/4 | 0.5 (0.06-1.90); in-window 2/2 |
| layered spoof (3 layers) at touch | 0.5 (0.06-1.90); in-window 2/2 | 2.9 (1.45-5.18); in-window 11/11 | 0.3 (0.01-1.47); in-window 0/1 |
| quote stuffing, touch flicker (50k/s) | 0.3 (0.01-1.47); in-window 0/1 | 0.8 (0.16-2.31); in-window 0/3 | 0.0 (0.00-0.97); in-window 0/0 |
| quote stuffing, deep (50k/s) | 0.3 (0.01-1.47); in-window 0/1 | 0.8 (0.16-2.31); in-window 0/3 | 0.0 (0.00-0.97); in-window 0/0 |

### BOCPD

| scenario | VOL (grid RV) | FLOW naive (raw OFI sum, no winsorising) | FLOW hardened (winsorised OFI, t-stat) |
|---|---|---|---|
| no manipulation | 1.3 (0.43-3.07); in-window 0/5 | 1.6 (0.58-3.44); in-window 0/6 | 0.8 (0.16-2.31); in-window 0/3 |
| spoof AT touch (50x, 1 s, 12/min) | 0.8 (0.16-2.31); in-window 1/3 | 2.9 (1.45-5.18); in-window 9/11 | 1.1 (0.29-2.70); in-window 4/4 |
| spoof 3 ticks behind touch | 0.3 (0.01-1.47); in-window 1/1 | 0.5 (0.06-1.90); in-window 1/2 | 0.3 (0.01-1.47); in-window 1/1 |
| layered spoof (3 layers) at touch | 1.6 (0.58-3.44); in-window 5/6 | 1.8 (0.74-3.80); in-window 4/7 | 0.5 (0.06-1.90); in-window 2/2 |
| quote stuffing, touch flicker (50k/s) | 0.8 (0.16-2.31); in-window 0/3 | 0.5 (0.06-1.90); in-window 0/2 | 0.3 (0.01-1.47); in-window 0/1 |
| quote stuffing, deep (50k/s) | 1.3 (0.43-3.07); in-window 0/5 | 1.6 (0.58-3.44); in-window 0/6 | 0.3 (0.01-1.47); in-window 0/1 |
