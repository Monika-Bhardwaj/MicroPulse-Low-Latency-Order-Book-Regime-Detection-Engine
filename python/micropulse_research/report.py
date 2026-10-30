"""Render reports/calibration_results.json as markdown tables (stdout)."""
import json
import sys


def fmt_ci(ci):
    return f"{ci[0]:.2f}-{ci[1]:.2f}"


def main(path):
    r = json.load(open(path))
    cfg = r["config"]
    o = []
    o.append("# Calibration run (auto-generated)\n")
    o.append(f"{len(cfg['cal_seeds'])} calibration seeds and {len(cfg['test_seeds'])} disjoint held-out seeds per class, {cfg['duration_s']:.0f} s each; "
             f"{r['n_changes_cal']} labeled changes in calibration, {r['n_changes_test']} in held-out. FP budget {cfg['target_fp_per_hour']}/hour. Runtime {r['runtime_s']:.0f} s.\n")
    o.append("## Calibration sweep (vol channel, calibration seeds, pooled clean+hostile null)\n")
    o.append("### CUSUM (k = 0.5)\n")
    o.append("| h | null FP / hour | alarms / hours | recall (changes detected within 45 s) | mean delay (s) |\n|---|---|---|---|---|")
    for x in r["sweep_calibration"]["x_vol"]["cusum"]:
        o.append(f"| {x['param']} | {x['fp_per_h']:.2f} | {x['fp_k']} / {x['hours']:.1f} | {x['recall']:.2f} | {x['mean_delay']:.1f} |")
    o.append("\n### BOCPD (best 12 settings by recall among those meeting the budget, then 3 most sensitive that do not)\n")
    rows = r["sweep_calibration"]["x_vol"]["bocpd"]
    ok = sorted([x for x in rows if x["fp_per_h"] <= cfg["target_fp_per_hour"]], key=lambda x: -x["recall"])[:12]
    notok = sorted([x for x in rows if x["fp_per_h"] > cfg["target_fp_per_hour"]], key=lambda x: -x["recall"])[:3]
    o.append("| hazard lambda | k_short | clip (sd) | threshold | null FP / hour | recall | mean delay (s) | meets budget |\n|---|---|---|---|---|---|---|---|")
    for x in ok + notok:
        lam, k, z, thr = x["param"]
        o.append(f"| {lam:.0f} | {k} | {z} | {thr} | {x['fp_per_h']:.2f} | {x['recall']:.2f} | {x['mean_delay']:.1f} | {'yes' if x['fp_per_h'] <= cfg['target_fp_per_hour'] else 'no'} |")
    o.append(f"\nChosen operating points: CUSUM h = {r['chosen']['cusum']}; BOCPD (lambda, k_short, clip, threshold) = {tuple(r['chosen']['bocpd'])}.\n")
    o.append("## Held-out evaluation (disjoint seeds, thresholds frozen)\n")
    o.append("| detector | null set | null exposure (h) | false alarms | **FP / hour** (95% CI) | changes | recall | mean delay (s) | p90 delay (s) |\n|---|---|---|---|---|---|---|---|---|")
    for kind, v in r["heldout"].items():
        for ds in ("clean", "hostile"):
            e = v[ds]
            o.append(f"| {kind} | {ds} | {e['null_hours']:.1f} | {e['null_fp_k']} | **{e['null_fp_per_h']:.2f}** ({fmt_ci(e['null_fp_ci'])}) | {e['n_changes']} | {e['recall']:.2f} | {e['delay_mean']:.1f} | {e['delay_p90']:.1f} |")
    o.append("\nFalse alarms inside the labeled-change runs (outside every 45 s detection window): " +
             "; ".join(f"{k}: {v['hostile']['fa_in_change_runs']} in {v['hostile']['fa_hours']:.1f} h = {v['hostile']['fa_per_h_change_runs']:.2f}/h (CI {fmt_ci(v['hostile']['fa_ci'])})" for k, v in r["heldout"].items()) + "\n")
    o.append("### Recall by shift magnitude (held-out)\n")
    o.append("| detector | shift (sigma ratio) | n | recall | mean delay (s) |\n|---|---|---|---|---|")
    for kind, v in r["heldout"].items():
        for mag, d in v["hostile"]["by_magnitude"].items():
            o.append(f"| {kind} | {mag} | {d['n']} | {d['recall']:.2f} | {d['delay']:.1f} |")
    o.append("\n## Manipulation robustness (held-out seeds; thresholds calibrated on the CLEAN null only)\n")
    o.append("### Detector-input distortion (detector-independent, high power)\n")
    o.append("For every spoof episode (or, for the no-manipulation / stuffing scenarios, a control window of the same length at random times) we take the largest robust z-score (|x - median| / 1.4826 MAD) of the detector input over the window. Reported: share of windows with max z > 4, > 6, and the median / 95th percentile of the window maximum.\n")
    o.append("| scenario | statistic | windows | P(max z > 4) | P(max z > 6) | median max z | p95 max z |\n|---|---|---|---|---|---|---|")
    for key, d in r.get("distortion", {}).items():
        sc, v = key.split("|")
        o.append(f"| {sc}{' (control windows)' if d['control'] else ''} | {v} | {d['n']} | {d['p_gt4']:.3f} | {d['p_gt6']:.3f} | {d['median']:.2f} | {d['p95']:.2f} |")
    o.append("\n### Alarms\n")
    o.append("Each cell = false alarms per hour (95% Poisson CI) in 8 x 1800 s runs; `in-window` = alarms that landed inside a spoof episode (+5 s). Thresholds per statistic/detector were set to <= 1 FP/hour on clean calibration data.\n")
    scen = []
    for key in r["manipulation"]:
        sc = key.split("|")[0]
        if sc not in scen:
            scen.append(sc)
    variants = []
    for key in r["manipulation"]:
        v = key.split("|")[1]
        if v not in variants:
            variants.append(v)
    for kind in ("cusum", "bocpd"):
        o.append(f"\n### {kind.upper()}\n")
        o.append("| scenario | " + " | ".join(variants) + " |\n|---|" + "---|" * len(variants))
        for sc in scen:
            cells = []
            for v in variants:
                m = r["manipulation"][f"{sc}|{v}|{kind}"]
                cells.append(f"{m['per_h']:.1f} ({fmt_ci(m['ci'])}); in-window {m['in_spoof_window']}/{m['alarms']}")
            o.append(f"| {sc} | " + " | ".join(cells) + " |")
    return "\n".join(o)


if __name__ == "__main__":
    print(main(sys.argv[1] if len(sys.argv) > 1 else "reports/calibration_results.json"))
