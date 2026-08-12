"""
Run pii_ner_eval.py across multiple PII-detection NER models against the same
corpus and print a combined comparison table.

Each model is run as its own subprocess of pii_ner_eval.py, so it keeps its
usual standalone log/JSON backup under <corpus-dir>/runs/ - this script adds
nothing beyond collecting those JSON summaries and reporting them side by
side. Reusable for future, larger comparisons: point --corpus-dir at a bigger
corpus (e.g. a 20k-snapshot build) and rerun the same command.

Usage:
    python test/eval/run_pii_ner_comparison.py [--corpus-dir DIR] [--models MODEL [MODEL ...]] [--limit N]

Defaults:
    --corpus-dir test/conformance/routing/1/l2_pii_nemotron
    --models     llm-semantic-router/mmbert32k-pii-detector-merged
                 OpenMed/privacy-filter-multilingual
                 ab-ai/pii_model
    --limit      0  (all cases; pass e.g. 20 for a smoke test across all models)

Examples:
    # Smoke test all 3 default models on 20 cases before a full run
    python test/eval/run_pii_ner_comparison.py --limit 20

    # Only the two new models, full corpus (skip re-running mmbert32k)
    python test/eval/run_pii_ner_comparison.py --models OpenMed/privacy-filter-multilingual ab-ai/pii_model

    # Later: same comparison against a larger corpus
    python test/eval/run_pii_ner_comparison.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

DEFAULT_MODELS = [
    "llm-semantic-router/mmbert32k-pii-detector-merged",
    "OpenMed/privacy-filter-multilingual",
    "ab-ai/pii_model",
]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--corpus-dir",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_nemotron"
        ),
    )
    p.add_argument("--models", nargs="+", default=DEFAULT_MODELS)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--max-length", type=int, default=8192)
    p.add_argument("--device", default=None)
    return p.parse_args()


def run_one(model: str, args: argparse.Namespace) -> dict | None:
    script = Path(__file__).parent / "pii_ner_eval.py"
    cmd = [
        sys.executable,
        str(script),
        "--corpus-dir",
        args.corpus_dir,
        "--model",
        model,
        "--max-length",
        str(args.max_length),
    ]
    if args.limit:
        cmd += ["--limit", str(args.limit)]
    if args.device:
        cmd += ["--device", args.device]

    print(f"\n{'=' * 70}\nRunning {model}\n{'=' * 70}")
    # pii_ner_eval.py exits 1 on any miss/FP, which is a normal completed
    # result in this series, not a crash - don't treat it as a driver failure.
    subprocess.run(cmd)

    runs_dir = Path(args.corpus_dir) / "runs"
    slug = model.split("/")[-1]
    candidates = sorted(
        runs_dir.glob(f"ner_{slug}_*.json"), key=lambda p: p.stat().st_mtime
    )
    if not candidates:
        print(f"WARNING: no JSON summary found for {model} under {runs_dir}")
        return None
    return json.loads(candidates[-1].read_text(encoding="utf-8"))


def main() -> None:
    args = parse_args()
    summaries: list[dict] = []
    for model in args.models:
        summary = run_one(model, args)
        if summary:
            summaries.append(summary)

    if not summaries:
        print("\nNo successful runs to compare.", file=sys.stderr)
        sys.exit(1)

    print(f"\n{'=' * 90}")
    print("COMBINED COMPARISON")
    print(f"{'=' * 90}")
    header = (
        f"{'Model':<48} {'Recall':>8} {'Miss':>6} {'FP':>4} "
        f"{'Runtime':>10} {'Cases/s':>8}"
    )
    print(header)
    print("-" * len(header))
    for s in summaries:
        counts = s["counts"]
        elapsed = s.get("elapsed_seconds", 0.0)
        rate = s["n_cases"] / elapsed if elapsed > 0 else 0.0
        print(
            f"{s['model']:<48} {s['recall']:>7.1%} {counts['FN']:>6} "
            f"{counts['FP']:>4} {elapsed:>9.0f}s {rate:>8.2f}"
        )
    print()

    combined_path = Path(args.corpus_dir) / "runs" / "ner_comparison_latest.json"
    combined_path.write_text(
        json.dumps(
            {
                "corpus_dir": args.corpus_dir,
                "models": args.models,
                "limit": args.limit,
                "summaries": summaries,
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    print(f"Combined summary -> {combined_path}")


if __name__ == "__main__":
    main()
