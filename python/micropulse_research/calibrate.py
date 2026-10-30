"""Regime-detector calibration and adversarial-robustness experiments.

Protocol
--------
* Data: labeled SYNTHETIC regimes (exact change points from the generator). Real Binance data
  could not be captured in the build sandbox (no outbound access), so the thresholds produced
  here are a calibration of the *pipeline mechanics*, not of a live market. Re-run this script
  on a recorded capture with hand/rule-labeled changes before trusting any absolute number.
* Calibration seeds (1..N) choose thresholds; disjoint HELD-OUT seeds (101..) report results.
* Operating point: smallest threshold whose false-alarm rate on pooled null data
  (clean + hostile) is <= TARGET_FP_PER_HOUR on the calibration seeds.
* A detection = first alarm within (tau, tau+T] of a true change; any other alarm after the
  warm-up is a false alarm. Alarms closer than REFRACTORY seconds are merged.
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import numpy as np
from scipy.stats import chi2

from . import _micropulse as mp

LEVELS = [(1.0, 1.0), (2.0, 1.5), (4.0, 2.5)]  # (sigma ticks/sqrt(s), churn-rate multiplier)
DUR = 1800.0
BASE_RATE = 800.0
WARMUP_EXCL = 90.0
T_DETECT = 45.0
REFRACTORY = 10.0
TARGET_FP_PER_HOUR = 1.0
CUSUM_H = [4, 5, 6, 7, 8, 10, 12, 14, 17, 20, 25]
BOCPD_THR = [0.9, 0.97, 0.99, 0.995, 0.999, 0.9995]
# structure grid searched on the calibration seeds: (hazard_lambda bars, k_short bars, observation clip in sd)
BOCPD_STRUCT = [(lam, k, z) for lam in (1000.0, 3000.0) for k in (20, 40, 80) for z in (2.5, 3.0)]

HOSTILE = dict(sigma_mod_amp=0.25, sigma_mod_period_s=600.0, act_burst_per_min=2.0, act_burst_mult=3.0,
               act_burst_len_s=5.0, spoof_per_min=6.0, spoof_depth_ticks=0, spoof_qty_mult=50.0, spoof_hold_s=1.0,
               stuff_per_min=2.0, stuff_len_s=0.5, stuff_rate=20000.0, stuff_mode=1)


# ------------------------------------------------------------------ simulation --
def make_regimes(rng: np.random.Generator, dur=DUR, dwell_min=90.0, dwell_extra=90.0):
    lvl = int(rng.integers(3))
    regs = [(0.0, *LEVELS[lvl])]
    meta = []  # (tau, sigma_before, sigma_after)
    t = 0.0
    while True:
        t += dwell_min + rng.exponential(dwell_extra)
        if t > dur - 60:
            break
        new = int(rng.choice([i for i in range(3) if i != lvl]))
        meta.append((t, LEVELS[lvl][0], LEVELS[new][0]))
        lvl = new
        regs.append((t, *LEVELS[lvl]))
    return regs, meta


def run_sim(cfg: dict, ofi_cap_mult: float = 8.0, max_levels: int = 256):
    e = mp.Engine(max_levels=max_levels, record_bars=True, ofi_cap_mult=ofi_cap_mult)
    out = mp.simulate(cfg, e)
    b = e.bars()
    t = (np.asarray(b["t_end_ns"]) - out["start_ns"]) / 1e9
    ofi, ofi_sq = np.asarray(b["ofi"]), np.asarray(b["ofi_sq"])
    return dict(
        t=t, x_vol=np.asarray(b["x_vol"]), x_raw=ofi,
        x_tstat=np.where(ofi_sq > 0, ofi / np.sqrt(np.maximum(ofi_sq, 1e-300)), 0.0),
        spoof=np.asarray(out["spoof_windows_ns"]) / 1e9 - out["start_ns"] / 1e9,
        stuff=np.asarray(out["stuff_windows_ns"]) / 1e9 - out["start_ns"] / 1e9,
        messages=int(out["messages"]),
    )


def sim_cfg(seed, regs=None, nuisance=None, **kw):
    c = dict(seed=seed, duration_s=DUR, base_rate=BASE_RATE, regimes=regs or [(0.0, 2.0, 1.5)])
    if nuisance:
        c.update(nuisance)
    c.update(kw)
    return c


# ------------------------------------------------------------------- detectors --
def alarm_times(t, alarm_flags):
    a = t[np.nonzero(alarm_flags)[0]]
    out = []
    for x in a:
        if not out or x - out[-1] >= REFRACTORY:
            out.append(float(x))
    return np.array(out)


def run_det(kind, x, param):
    if kind == "cusum":
        _, a = mp.run_cusum(x, 0.5, float(param), 120, 60)
    else:  # param = (hazard_lambda, k_short, zclip, thr)
        lam, k, z, thr = param
        _, a = mp.run_bocpd(x, float(lam), 200, int(k), float(thr), 40, float(z))
    return np.asarray(a)


def poisson_ci(k, hours, conf=0.95):
    a = 1 - conf
    lo = 0.0 if k == 0 else chi2.ppf(a / 2, 2 * k) / 2
    hi = chi2.ppf(1 - a / 2, 2 * (k + 1)) / 2
    return lo / hours, hi / hours


def fp_hours(alarms_per_run, runs):
    k = 0
    hours = 0.0
    for a, r in zip(alarms_per_run, runs):
        k += int(np.sum(a >= WARMUP_EXCL))
        hours += (r["t"][-1] - WARMUP_EXCL) / 3600.0
    return k, hours


def eval_changes(alarms, changes, t_end):
    """changes: list of (tau, s_before, s_after). Returns (detections[(delay,s_b,s_a) or None], false_alarm_count, exposure_h)."""
    res, covered = [], np.zeros(len(alarms), bool)
    for tau, sb, sa in changes:
        hit = np.nonzero((alarms > tau) & (alarms <= tau + T_DETECT))[0]
        if len(hit):
            res.append((alarms[hit[0]] - tau, sb, sa))
            covered[hit] = True
        else:
            res.append((None, sb, sa))
    # alarms inside a detection window (after any change) are not false alarms
    fa = int(np.sum((~covered) & (alarms >= WARMUP_EXCL)))
    # exposure excludes the detection windows
    exposure = (t_end - WARMUP_EXCL - T_DETECT * len(changes)) / 3600.0
    return res, fa, exposure


# ----------------------------------------------------------------- experiments --
def collect(seeds, kind, **kw):
    runs, metas = [], []
    for s in seeds:
        if kind == "null_clean":
            r = run_sim(sim_cfg(s))
            m = []
        elif kind == "null_hostile":
            r = run_sim(sim_cfg(s, nuisance=HOSTILE))
            m = []
        elif kind == "change_hostile":
            regs, m = make_regimes(np.random.default_rng(1000 + s))
            r = run_sim(sim_cfg(s, regs=regs, nuisance=HOSTILE))
        else:
            raise ValueError(kind)
        runs.append(r)
        metas.append(m)
    return runs, metas


def sweep(runs_null, runs_change, metas_change, chan="x_vol"):
    """Return {detector: [{param, fp_per_h, fp_k, hours, recall, mean_delay}]} on the given runs."""
    table = {}
    bparams = [(lam, k, z, thr) for (lam, k, z) in BOCPD_STRUCT for thr in BOCPD_THR]
    for kind, params in (("cusum", CUSUM_H), ("bocpd", bparams)):
        rows = []
        for p in params:
            an = [alarm_times(r["t"], run_det(kind, r[chan], p)) for r in runs_null]
            k, h = fp_hours(an, runs_null)
            det, fa, expo = [], 0, 0.0
            for r, m in zip(runs_change, metas_change):
                a = alarm_times(r["t"], run_det(kind, r[chan], p))
                d, f, e = eval_changes(a, m, r["t"][-1])
                det += d
                fa += f
                expo += e
            hit = [x[0] for x in det if x[0] is not None]
            rows.append(dict(param=p, fp_per_h=k / h, fp_k=k, hours=h, recall=len(hit) / max(1, len(det)),
                             n_changes=len(det), mean_delay=float(np.mean(hit)) if hit else float("nan"),
                             fa_in_change_runs_per_h=fa / max(expo, 1e-9)))
        table[kind] = rows
    return table


def pick(rows, target=TARGET_FP_PER_HOUR):
    """Among settings meeting the FP budget on the calibration null, take the one with the highest recall
    (ties: lower FP, then earlier detection). If none meets it, take the lowest-FP setting."""
    ok = [r for r in rows if r["fp_per_h"] <= target]
    if not ok:
        return min(rows, key=lambda r: r["fp_per_h"])["param"]
    best = max(ok, key=lambda r: (round(r["recall"], 4), -r["fp_per_h"], -(r["mean_delay"] if r["mean_delay"] == r["mean_delay"] else 1e9)))
    return best["param"]


def evaluate_at(kind, p, runs_null, runs_change, metas_change, chan):
    an = [alarm_times(r["t"], run_det(kind, r[chan], p)) for r in runs_null]
    k, h = fp_hours(an, runs_null)
    lo, hi = poisson_ci(k, h)
    det, fa, expo = [], 0, 0.0
    for r, m in zip(runs_change, metas_change):
        a = alarm_times(r["t"], run_det(kind, r[chan], p))
        d, f, e = eval_changes(a, m, r["t"][-1])
        det += d; fa += f; expo += e
    hit = [x[0] for x in det if x[0] is not None]
    by = {}
    for d, sb, sa in det:
        key = f"{'up' if sa > sb else 'down'} x{max(sa, sb) / min(sa, sb):.0f}"
        by.setdefault(key, []).append(d)
    by_mag = {k2: dict(n=len(v), recall=sum(x is not None for x in v) / len(v),
                       delay=float(np.mean([x for x in v if x is not None])) if any(x is not None for x in v) else float("nan"))
              for k2, v in sorted(by.items())}
    fa_hi = poisson_ci(fa, max(expo, 1e-9))
    return dict(param=p, null_fp_k=k, null_hours=h, null_fp_per_h=k / h, null_fp_ci=(lo, hi),
                n_changes=len(det), recall=len(hit) / max(1, len(det)),
                delay_mean=float(np.mean(hit)) if hit else float("nan"),
                delay_p90=float(np.percentile(hit, 90)) if hit else float("nan"),
                fa_in_change_runs=fa, fa_hours=expo, fa_per_h_change_runs=fa / max(expo, 1e-9), fa_ci=fa_hi, by_magnitude=by_mag)


def spoof_stuff_experiment(cal_seeds, test_seeds, thresholds):
    """Do manipulative flows fool the detectors? thresholds: {(variant, detector): param} calibrated on clean null."""
    scenarios = {
        "no manipulation": {},
        "spoof AT touch (50x, 1 s, 12/min)": dict(spoof_per_min=12.0, spoof_depth_ticks=0, spoof_qty_mult=50.0, spoof_hold_s=1.0),
        "spoof 3 ticks behind touch": dict(spoof_per_min=12.0, spoof_depth_ticks=3, spoof_qty_mult=50.0, spoof_hold_s=1.0),
        "layered spoof (3 layers) at touch": dict(spoof_per_min=12.0, spoof_depth_ticks=0, spoof_qty_mult=50.0, spoof_hold_s=1.0, spoof_layers=3),
        "quote stuffing, touch flicker (50k/s)": dict(stuff_per_min=4.0, stuff_len_s=0.5, stuff_rate=50000.0, stuff_mode=1),
        "quote stuffing, deep (50k/s)": dict(stuff_per_min=4.0, stuff_len_s=0.5, stuff_rate=50000.0, stuff_mode=0),
    }
    variants = {  # name -> (ofi_cap_mult, series)
        "VOL (grid RV)": (8.0, "x_vol"),
        "FLOW naive (raw OFI sum, no winsorising)": (0.0, "x_raw"),
        "FLOW hardened (winsorised OFI, t-stat)": (8.0, "x_tstat"),
    }
    out = {}
    distortion = {}
    rng = np.random.default_rng(5)
    for sname, extra in scenarios.items():
        for vname, (cap, ser) in variants.items():
            runs = [run_sim(sim_cfg(s, **extra), ofi_cap_mult=cap) for s in test_seeds]
            # --- detector-independent measure: how extreme does the detector INPUT get inside spoof episodes? ---
            maxz = []
            for r in runs:
                x = r[ser]
                med = np.median(x)
                mad = 1.4826 * np.median(np.abs(x - med)) + 1e-12
                z = np.abs(x - med) / mad
                t = r["t"]
                wins = r["spoof"] if len(r["spoof"]) else np.stack([c0 := rng.uniform(WARMUP_EXCL, DUR - 5, 360), c0 + 1.0], axis=1)
                for w0, w1 in wins:
                    sel = (t > w0) & (t <= w1 + 0.5)
                    if sel.any():
                        maxz.append(float(z[sel].max()))
            mz = np.array(maxz)
            distortion[(sname, vname)] = dict(n=int(len(mz)), p_gt4=float(np.mean(mz > 4)), p_gt6=float(np.mean(mz > 6)),
                                              median=float(np.median(mz)), p95=float(np.percentile(mz, 95)),
                                              control=not len(runs[0]["spoof"]))
            hours = sum((r["t"][-1] - WARMUP_EXCL) / 3600.0 for r in runs)
            win = [r["spoof"] for r in runs]
            for kind in ("cusum", "bocpd"):
                p = thresholds[(vname, kind)]
                al = [alarm_times(r["t"], run_det(kind, r[ser], p)) for r in runs]
                k = sum(int(np.sum(a >= WARMUP_EXCL)) for a in al)
                # alarms landing in [spoof start, spoof end + 5 s]
                inw = 0
                for a, w in zip(al, win):
                    for x in a[a >= WARMUP_EXCL]:
                        if len(w) and np.any((x >= w[:, 0]) & (x <= w[:, 1] + 5.0)):
                            inw += 1
                lo, hi = poisson_ci(k, hours)
                out[(sname, vname, kind)] = dict(alarms=k, hours=hours, per_h=k / hours, ci=(lo, hi), in_spoof_window=inw, param=p)
    return out, distortion


def _first_ok(rows):
    ok = [r for r in rows if r["fp_per_h"] <= TARGET_FP_PER_HOUR]
    return ok[0]["param"] if ok else rows[-1]["param"]  # least strict setting meeting the budget (params ascend in strictness)


def calibrate_flow_thresholds(cal_seeds, bocpd_struct):
    """Per (variant, detector): smallest threshold with <= TARGET FP/h on CLEAN null (calibration seeds)."""
    th = {}
    for vname, cap, ser in (("VOL (grid RV)", 8.0, "x_vol"), ("FLOW naive (raw OFI sum, no winsorising)", 0.0, "x_raw"),
                            ("FLOW hardened (winsorised OFI, t-stat)", 8.0, "x_tstat")):
        runs = [run_sim(sim_cfg(s), ofi_cap_mult=cap) for s in cal_seeds]
        for kind, params in (("cusum", CUSUM_H), ("bocpd", [bocpd_struct + (t,) for t in BOCPD_THR])):
            rows = []
            for p in params:
                k, h = fp_hours([alarm_times(r["t"], run_det(kind, r[ser], p)) for r in runs], runs)
                rows.append(dict(param=p, fp_per_h=k / h))
            th[(vname, kind)] = _first_ok(rows)
    return th


# ----------------------------------------------------------------------- main --
def main(n_cal=8, n_test=8, out_dir="reports"):
    t0 = time.time()
    cal = list(range(1, n_cal + 1))
    test = list(range(101, 101 + n_test))
    print(f"[calibrate] collecting calibration runs ({n_cal} per class)...", flush=True)
    cal_null_clean, _ = collect(cal, "null_clean")
    cal_null_host, _ = collect(cal, "null_hostile")
    cal_chg, cal_meta = collect(cal, "change_hostile")
    print(f"[calibrate] {time.time() - t0:.0f}s: sweeping thresholds", flush=True)
    cal_null = cal_null_clean + cal_null_host
    sweeps = {"x_vol": sweep(cal_null, cal_chg, cal_meta, "x_vol")}
    chosen = {k: pick(v) for k, v in sweeps["x_vol"].items()}
    chosen = {k: (list(v) if isinstance(v, tuple) else v) for k, v in chosen.items()}
    print("[calibrate] chosen operating points:", chosen, flush=True)
    print("[calibrate] held-out evaluation", flush=True)
    test_null_clean, _ = collect(test, "null_clean")
    test_null_host, _ = collect(test, "null_hostile")
    test_chg, test_meta = collect(test, "change_hostile")
    heldout = {}
    for kind, p in chosen.items():
        heldout[kind] = dict(
            clean=evaluate_at(kind, p, test_null_clean, test_chg, test_meta, "x_vol"),
            hostile=evaluate_at(kind, p, test_null_host, test_chg, test_meta, "x_vol"),
        )
    print(f"[calibrate] {time.time() - t0:.0f}s: manipulation experiments", flush=True)
    th = calibrate_flow_thresholds(cal, tuple(chosen['bocpd'][:3]))
    manip, distortion = spoof_stuff_experiment(cal, test, th)
    res = dict(config=dict(duration_s=DUR, base_rate=BASE_RATE, levels=LEVELS, warmup_excl=WARMUP_EXCL, t_detect=T_DETECT,
                           refractory=REFRACTORY, target_fp_per_hour=TARGET_FP_PER_HOUR, cal_seeds=cal, test_seeds=test),
               sweep_calibration=sweeps, chosen=chosen, heldout=heldout,
               manipulation={f"{a}|{b}|{c}": v for (a, b, c), v in manip.items()},
               distortion={f"{a}|{b}": v for (a, b), v in distortion.items()},
               manipulation_thresholds={f"{a}|{b}": v for (a, b), v in th.items()},
               n_changes_cal=sum(len(m) for m in cal_meta), n_changes_test=sum(len(m) for m in test_meta),
               runtime_s=time.time() - t0)
    Path(out_dir).mkdir(exist_ok=True)
    Path(out_dir, "calibration_results.json").write_text(json.dumps(res, indent=1, default=float))
    print(f"[calibrate] done in {time.time() - t0:.0f}s -> {out_dir}/calibration_results.json")
    return res


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    main(n, n)
