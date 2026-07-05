#!/usr/bin/env python3
"""
frag_ab.py -- A/B fragmentation comparison for mymalloc (Phase 11/12).

Reads one or two /proc/mymalloc/trace CSV dumps and reports fragmentation
statistics. With two runs (baseline vs. hints) it prints a side-by-side
comparison and the improvement attributable to the ML hints.

Trace schema (from module/mymalloc.c trace_show):
    timestamp_ns,op,order,addr,free0..free10,frag_score_pct
frag_score_pct: 0 = best (order-10 block free), 100 = worst (fully fragmented).

Stdlib only -- no numpy/pandas/matplotlib required.

Usage:
    python3 frag_ab.py trace_baseline.csv trace_hints.csv
    python3 frag_ab.py --baseline base.csv --hints hints.csv
    python3 frag_ab.py single_run.csv            # summary of one run
    python3 frag_ab.py -t 40 base.csv hints.csv  # custom "high frag" threshold
"""
import argparse
import csv
import statistics
import sys

SPARK = "▁▂▃▄▅▆▇█"


def load(path):
    """Return list of (timestamp_ns, op, frag_pct) from a trace CSV."""
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None or "frag_score_pct" not in reader.fieldnames:
            raise ValueError(
                f"{path}: no 'frag_score_pct' column found "
                f"(is this a /proc/mymalloc/trace dump?)"
            )
        for r in reader:
            try:
                ts = int(r["timestamp_ns"])
                frag = int(r["frag_score_pct"])
            except (KeyError, ValueError, TypeError):
                continue  # skip malformed / partial lines
            rows.append((ts, r.get("op", "?"), frag))
    return rows


def time_weighted_mean(rows):
    """Average fragmentation *experienced over time*.

    Each event's frag holds until the next event, so weight by the dt to the
    following event. This is the honest metric: it doesn't let a burst of
    rapid-fire events at one frag level dominate a long quiet stretch.
    """
    if len(rows) < 2:
        return float(rows[0][2]) if rows else float("nan")
    num = den = 0.0
    for (t0, _, f0), (t1, _, _) in zip(rows, rows[1:]):
        dt = t1 - t0
        if dt < 0:
            dt = 0  # guard against non-monotonic timestamps
        num += f0 * dt
        den += dt
    if den == 0:  # all events at same timestamp -> fall back to simple mean
        return statistics.fmean(f for _, _, f in rows)
    return num / den


def pct(sorted_vals, p):
    """Nearest-rank percentile of an already-sorted list."""
    if not sorted_vals:
        return float("nan")
    k = max(0, min(len(sorted_vals) - 1, round((p / 100) * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def sparkline(rows, width=60):
    """Downsampled ASCII trajectory of frag over the run (0-100 -> ▁..█)."""
    fr = [f for _, _, f in rows]
    if not fr:
        return ""
    if len(fr) > width:
        step = len(fr) / width
        fr = [fr[min(len(fr) - 1, int(i * step))] for i in range(width)]
    return "".join(SPARK[min(len(SPARK) - 1, v * (len(SPARK) - 1) // 100)] for v in fr)


def summarize(rows, threshold):
    fr = [f for _, _, f in rows]
    frs = sorted(fr)
    span_ns = (rows[-1][0] - rows[0][0]) if len(rows) >= 2 else 0
    return {
        "events": len(rows),
        "span_ms": span_ns / 1e6,
        "mean": statistics.fmean(fr) if fr else float("nan"),
        "twmean": time_weighted_mean(rows),
        "median": statistics.median(fr) if fr else float("nan"),
        "p95": pct(frs, 95),
        "peak": max(fr) if fr else float("nan"),
        "final": fr[-1] if fr else float("nan"),
        "pct_above": (100.0 * sum(1 for v in fr if v > threshold) / len(fr)) if fr else float("nan"),
        "spark": sparkline(rows),
    }


def print_single(name, s, threshold):
    print(f"\n=== {name} ===")
    print(f"  events recorded     : {s['events']}")
    print(f"  wall span           : {s['span_ms']:.2f} ms")
    print(f"  frag  time-weighted : {s['twmean']:.2f} / 100   <- headline number")
    print(f"  frag  simple mean   : {s['mean']:.2f} / 100")
    print(f"  frag  median        : {s['median']:.1f}")
    print(f"  frag  p95           : {s['p95']:.1f}")
    print(f"  frag  peak          : {s['peak']:.0f}")
    print(f"  frag  final         : {s['final']:.0f}")
    print(f"  time above {threshold:>3}%     : {s['pct_above']:.1f}% of events")
    print(f"  trajectory          : {s['spark']}")


def improved(base, hint, key, lower_is_better=True):
    b, h = base[key], hint[key]
    if b == 0:
        rel = 0.0 if h == 0 else float("inf")
    else:
        rel = (h - b) / b * 100.0
    delta = h - b
    good = (delta < 0) if lower_is_better else (delta > 0)
    arrow = "▼" if delta < 0 else ("▲" if delta > 0 else "=")
    tag = "better" if good else ("worse" if delta != 0 else "same")
    return b, h, delta, rel, arrow, tag


def print_compare(base, hint, threshold):
    print("\n" + "=" * 68)
    print("A/B FRAGMENTATION COMPARISON  (lower frag = better)")
    print("=" * 68)
    print_single("BASELINE  (MYMALLOC_HINTS_ENABLED=0)", base, threshold)
    print_single("HINTS     (MYMALLOC_HINTS_ENABLED=1)", hint, threshold)

    print("\n=== VERDICT ===")
    rows = [
        ("time-weighted mean frag", "twmean"),
        ("peak frag",               "peak"),
        ("p95 frag",                "p95"),
        ("final frag",              "final"),
        (f"% events above {threshold}%",   "pct_above"),
    ]
    print(f"  {'metric':<26}{'baseline':>10}{'hints':>10}{'delta':>9}{'rel':>9}")
    for label, key in rows:
        b, h, delta, rel, arrow, tag = improved(base, hint, key)
        rel_s = "  --  " if rel == float("inf") else f"{rel:+.1f}%"
        print(f"  {label:<26}{b:>10.2f}{h:>10.2f}{delta:>+9.2f}{rel_s:>9}  {arrow} {tag}")

    b_tw, h_tw = base["twmean"], hint["twmean"]
    if b_tw and h_tw <= b_tw:
        print(f"\n  => ML hints cut time-weighted fragmentation by "
              f"{(b_tw - h_tw) / b_tw * 100:.1f}% "
              f"({b_tw:.1f} -> {h_tw:.1f}).")
    elif b_tw:
        print(f"\n  => ML hints did NOT reduce fragmentation this run "
              f"({b_tw:.1f} -> {h_tw:.1f}). Check hint timing / dataset.")
    print()


def main(argv=None):
    ap = argparse.ArgumentParser(description="mymalloc A/B fragmentation analysis")
    ap.add_argument("files", nargs="*", help="1 trace CSV (summary) or 2 (baseline hints)")
    ap.add_argument("--baseline", help="baseline trace CSV (hints disabled)")
    ap.add_argument("--hints", help="hints-active trace CSV")
    ap.add_argument("-t", "--threshold", type=int, default=50,
                    help="'high fragmentation' threshold percent (default 50)")
    args = ap.parse_args(argv)

    base_path = args.baseline
    hint_path = args.hints
    if not base_path and not hint_path:
        if len(args.files) == 2:
            base_path, hint_path = args.files
        elif len(args.files) == 1:
            try:
                rows = load(args.files[0])
            except (OSError, ValueError) as e:
                print(f"error: {e}", file=sys.stderr)
                return 2
            if not rows:
                print("No usable rows found.", file=sys.stderr)
                return 2
            print_single(args.files[0], summarize(rows, args.threshold), args.threshold)
            return 0
        else:
            ap.error("give one CSV (summary) or two (--baseline/--hints or positional)")

    try:
        base_rows = load(base_path)
        hint_rows = load(hint_path)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if not base_rows or not hint_rows:
        print("error: one of the traces has no usable rows.", file=sys.stderr)
        return 2

    print_compare(summarize(base_rows, args.threshold),
                  summarize(hint_rows, args.threshold),
                  args.threshold)
    return 0


if __name__ == "__main__":
    sys.exit(main())
