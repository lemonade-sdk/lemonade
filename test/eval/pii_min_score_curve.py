"""
Turn pii_min_score_sweep.py's JSONL into the min_score tuning curve.

Each sweep record carries one number per case - the top non-"O" score the
router's rule aggregates to - so recall at any threshold is arithmetic over
that column, no re-inference. This prints:

  * the argmax baseline (what pii_ner_eval.py's safetensors run measured)
  * leak count / recall at each candidate min_score
  * the floor: cases the model misses under *any* threshold, because its best
    non-"O" score is ~0, which no threshold recovers
  * which PII categories the threshold-only leaks concentrate in

The false-positive side is NOT measurable here. l2_pii_nemotron_20k is 20000
PII cases and 1 benign case, so lowering min_score has no measurable cost in
this corpus and the curve below is one-sided by construction. Read it as "how
much recall is left on the table", never as "0.2 is the better threshold".

Usage:
    python test/eval/pii_min_score_curve.py [--sweep FILE] [--thresholds LIST] [--json OUT]
"""

import argparse
import json
from collections import Counter
from pathlib import Path

DEFAULT_SWEEP = (
    "test/conformance/routing/1/l2_pii_nemotron_20k/runs/min_score_sweep.jsonl"
)
DEFAULT_THRESHOLDS = "0.05,0.1,0.15,0.2,0.25,0.3,0.35,0.4,0.45,0.5,0.6,0.7,0.8,0.9"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--sweep", default=DEFAULT_SWEEP)
    p.add_argument("--thresholds", default=DEFAULT_THRESHOLDS)
    p.add_argument("--json", default=None, help="Also write the curve as JSON")
    p.add_argument("--top-categories", type=int, default=12)
    return p.parse_args()


def main() -> None:
    args = parse_args()
    records = [
        json.loads(line)
        for line in Path(args.sweep).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    pii = [r for r in records if r.get("pii_category")]
    benign = [r for r in records if not r.get("pii_category")]
    n = len(pii)
    if not n:
        raise SystemExit(f"no PII-bearing cases in {args.sweep}")

    print(f"Sweep      : {args.sweep}")
    print(f"PII cases  : {n}")
    print(f"Benign     : {len(benign)}  <- FP side is unmeasurable at this size")
    trunc = sum(1 for r in pii if r.get("truncated"))
    print(f"Truncated  : {trunc}")

    argmax_leaks = [r for r in pii if not r["argmax"]]
    print(
        f"\nargmax baseline (safetensors rule): {len(argmax_leaks)} leaks "
        f"({100 * len(argmax_leaks) / n:.3f}%), recall {100 * (1 - len(argmax_leaks) / n):.3f}%"
    )

    print("\nmin_score curve (router rule):")
    print(
        f"  {'min_score':>9}  {'leaks':>6}  {'leak rate':>9}  {'recall':>8}  vs argmax"
    )
    curve = []
    for t in [float(x) for x in args.thresholds.split(",")]:
        leaks = [r for r in pii if r["top"] < t]
        extra = len(leaks) - len(argmax_leaks)
        curve.append(
            {"min_score": t, "leaks": len(leaks), "recall": 1 - len(leaks) / n}
        )
        print(
            f"  {t:>9.2f}  {len(leaks):>6}  {100 * len(leaks) / n:>8.3f}%  "
            f"{100 * (1 - len(leaks) / n):>7.3f}%  {extra:+d}"
        )

    floor = [r for r in pii if r["top"] < 0.01]
    print(
        f"\nfloor: {len(floor)} cases score < 0.01 - no threshold recovers these "
        f"(genuine model misses)"
    )

    at_50 = [r for r in pii if r["top"] < 0.5]
    recoverable = [r for r in at_50 if r["top"] >= 0.01]
    print(
        f"of the {len(at_50)} leaks at min_score 0.5, {len(recoverable)} sit above the "
        f"floor and are threshold-recoverable"
    )
    if recoverable:
        vals = sorted(r["top"] for r in recoverable)
        q = lambda f: vals[min(int(f * len(vals)), len(vals) - 1)]
        print(
            f"  their scores: min {vals[0]:.4f}  p25 {q(0.25):.4f}  med {q(0.5):.4f}  "
            f"p75 {q(0.75):.4f}  max {vals[-1]:.4f}"
        )

    cats = Counter()
    for r in at_50:
        for c in r["pii_category"].split(","):
            if c:
                cats[c] += 1
    print(f"\nPII categories among the {len(at_50)} leaks at 0.5:")
    for cat, cnt in cats.most_common(args.top_categories):
        print(f"  {cnt:>4}  {cat}")

    labels = Counter(r["label"] for r in recoverable)
    print("\nnear-miss labels (best non-O label on threshold-recoverable leaks):")
    for lab, cnt in labels.most_common(10):
        print(f"  {cnt:>4}  {lab}")

    if args.json:
        Path(args.json).write_text(
            json.dumps(
                {
                    "sweep": args.sweep,
                    "n_pii": n,
                    "n_benign": len(benign),
                    "argmax_leaks": len(argmax_leaks),
                    "floor": len(floor),
                    "curve": curve,
                },
                indent=2,
            ),
            encoding="utf-8",
        )
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
