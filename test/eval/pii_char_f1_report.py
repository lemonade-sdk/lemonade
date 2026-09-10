"""
Re-score and compare character-level PII runs from their saved span files.

pii_char_f1_eval.py writes every predicted span to `<run>.spans.jsonl`. This
reads those back and recomputes the metrics offline, so the analyses that
follow the first run - a different threshold, per-label scoring, a partial
progress check while a run is still going - cost seconds instead of another
pass over 20,001 documents. Recording the spans is the fix for the reason this
work needed re-inference in the first place: no earlier run in the series ever
wrote down WHERE a model fired, only which label names it emitted.

It also runs on a run that is still in flight. A spans file is written
incrementally, so pointing this at a live run reports the numbers so far over
whatever prefix of the corpus has been scored - use --require-complete to
refuse a partial file instead.

Three views, from the same file:

  (default)   label-agnostic character F1 - the headline. Union every predicted
              span, union every gold span, compare character sets. No taxonomy.
  --by-label  per gold label, over gold characters carrying it. Answers "which
              categories does this model actually cover" in characters rather
              than in the document-level binary, which credits a model for
              firing anywhere in a document.
  --by-length gold spans bucketed by length. Short entities are where subword
              fragmentation costs the most.

Requirements: none beyond the standard library.

Usage:
    python test/eval/pii_char_f1_report.py [--corpus-dir DIR] [SPANS.jsonl ...]

Defaults:
    --corpus-dir  test/conformance/routing/1/l2_pii_nemotron_20k
    SPANS         every <corpus-dir>/runs/charf1_*.spans.jsonl

Examples:
    python test/eval/pii_char_f1_report.py
    python test/eval/pii_char_f1_report.py --by-label
    python test/eval/pii_char_f1_report.py --rule min_score
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("spans", nargs="*", help="charf1_*.spans.jsonl files")
    p.add_argument(
        "--corpus-dir",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_nemotron_20k"
        ),
    )
    p.add_argument(
        "--rule",
        default="primary",
        help="argmax | min_score | viterbi | primary (viterbi where the model "
        "has one, else argmax) | all",
    )
    p.add_argument("--by-label", action="store_true", help="Per gold label")
    p.add_argument("--by-length", action="store_true", help="By gold span length")
    p.add_argument(
        "--require-complete",
        action="store_true",
        help="Fail rather than report on a run still in progress",
    )
    p.add_argument("--json-out", default="", help="Write the table as JSON too")
    return p.parse_args()


def prf(tp: int, fp: int, fn: int) -> tuple[float, float, float]:
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    return (
        precision,
        recall,
        2 * precision * recall / (precision + recall) if precision + recall else 0.0,
    )


def chars(spans, text: str) -> set[int]:
    """Non-whitespace character indices a span list covers.

    Whitespace is dropped on both sides everywhere in this file, for the reason
    pii_char_f1_eval.py explains: subword tokenizers fold the leading space
    into a token, so counting it hands models free credit in amounts that vary
    by tokenizer.
    """
    out: set[int] = set()
    for start, end in spans:
        for i in range(start, min(end, len(text))):
            if not text[i].isspace():
                out.add(i)
    return out


def load_corpus(corpus_dir: Path) -> dict[str, dict]:
    cases_path = corpus_dir / "cases.jsonl"
    if not cases_path.exists():
        print(f"ERROR: {cases_path} not found", file=sys.stderr)
        sys.exit(1)
    corpus = {}
    for line in cases_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        case = json.loads(line)
        corpus[case["name"]] = {
            "text": case["request"]["messages"][0]["content"],
            "spans": case.get("pii_spans", []),
        }
    if not any(v["spans"] for v in corpus.values()):
        print(
            "ERROR: this corpus carries no gold `pii_spans`. Run "
            "annotate_gold_spans.py on it first.",
            file=sys.stderr,
        )
        sys.exit(1)
    return corpus


def score(records, corpus, rule):
    overall = {"tp": 0, "fp": 0, "fn": 0}
    per_label = defaultdict(lambda: {"tp": 0, "fn": 0})
    per_length = defaultdict(lambda: {"tp": 0, "fn": 0})
    macro = []
    doc = {"tp": 0, "fn": 0, "fp": 0, "tn": 0}
    missing = 0

    for record in records:
        case = corpus.get(record["name"])
        if case is None:
            missing += 1
            continue
        text = case["text"]
        available = record["spans"]
        key = rule
        if key == "primary":
            key = "viterbi" if available.get("viterbi") else "argmax"
        predicted = chars([tuple(s) for s in available.get(key, [])], text)
        gold = chars([(s["start"], s["end"]) for s in case["spans"]], text)

        tp, fp, fn = len(predicted & gold), len(predicted - gold), len(gold - predicted)
        overall["tp"] += tp
        overall["fp"] += fp
        overall["fn"] += fn
        if gold or predicted:
            macro.append(prf(tp, fp, fn)[2])
        has_gold, has_pred = bool(case["spans"]), bool(available.get(key))
        doc["tp" if has_gold and has_pred else
            "fn" if has_gold else
            "fp" if has_pred else "tn"] += 1

        for span in case["spans"]:
            span_chars = chars([(span["start"], span["end"])], text)
            hit = len(span_chars & predicted)
            per_label[span["label"]]["tp"] += hit
            per_label[span["label"]]["fn"] += len(span_chars) - hit
            length = len(span_chars)
            bucket = (
                "1-4" if length <= 4 else
                "5-9" if length <= 9 else
                "10-19" if length <= 19 else
                "20-39" if length <= 39 else "40+"
            )
            per_length[bucket]["tp"] += hit
            per_length[bucket]["fn"] += length - hit

    return {
        "overall": overall,
        "per_label": dict(per_label),
        "per_length": dict(per_length),
        "macro_f1": sum(macro) / len(macro) if macro else 0.0,
        "doc": doc,
        "n": len(records) - missing,
        "missing": missing,
    }


def main() -> None:
    args = parse_args()
    corpus_dir = Path(args.corpus_dir)
    corpus = load_corpus(corpus_dir)

    paths = [Path(p) for p in args.spans] or sorted(
        (corpus_dir / "runs").glob("charf1_*.spans.jsonl")
    )
    if not paths:
        print("ERROR: no charf1_*.spans.jsonl files found", file=sys.stderr)
        sys.exit(1)

    results = {}
    for path in paths:
        records = []
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    break  # a live run's last line can be half-written
        if not records:
            continue
        complete = len(records) >= len(corpus)
        if args.require_complete and not complete:
            print(
                f"ERROR: {path.name} covers {len(records):,} of {len(corpus):,} "
                "cases and --require-complete was given",
                file=sys.stderr,
            )
            sys.exit(1)
        model = path.name.split("_")[1]
        rules = (
            ["argmax", "min_score", "viterbi"] if args.rule == "all" else [args.rule]
        )
        for rule in rules:
            if rule not in ("primary", "argmax") and not records[0]["spans"].get(rule):
                continue
            label = model if len(rules) == 1 else f"{model}/{rule}"
            results[label] = {
                **score(records, corpus, rule),
                "path": str(path),
                "complete": complete,
                "coverage": len(records) / len(corpus),
            }

    gold_chars_total = sum(
        len(chars([(s["start"], s["end"]) for s in v["spans"]], v["text"]))
        for v in corpus.values()
    )
    prompt_chars_total = sum(
        len([c for c in v["text"] if not c.isspace()]) for v in corpus.values()
    )
    density = gold_chars_total / max(1, prompt_chars_total)

    print("=" * 84)
    print(f"CHARACTER-LEVEL COMPARISON - rule: {args.rule}")
    print("=" * 84)
    print(f"  corpus: {corpus_dir}  ({len(corpus):,} cases)")
    print()
    print(f"  {'model':<22} {'cases':>7} {'char P':>8} {'char R':>8} "
          f"{'char F1':>8} {'macro':>8} {'doc leak':>9}")
    for name, r in sorted(results.items(), key=lambda kv: -prf(**kv[1]["overall"])[2]):
        precision, recall, f1 = prf(**r["overall"])
        leak = r["doc"]["fn"] / max(1, r["doc"]["tp"] + r["doc"]["fn"])
        flag = "" if r["complete"] else f"  (partial {r['coverage']:.0%})"
        print(f"  {name:<22} {r['n']:>7,} {precision:>8.4f} {recall:>8.4f} "
              f"{f1:>8.4f} {r['macro_f1']:>8.4f} {leak:>9.4%}{flag}")
    print()
    print(f"  flag-everything floor  {'':>7} {density:>8.4f} {1.0:>8.4f} "
          f"{2 * density / (1 + density):>8.4f}")
    print()

    if args.by_label:
        labels = sorted(
            {label for r in results.values() for label in r["per_label"]},
            key=lambda label: -sum(
                r["per_label"].get(label, {}).get("tp", 0)
                + r["per_label"].get(label, {}).get("fn", 0)
                for r in results.values()
            ),
        )
        print("  CHARACTER RECALL BY GOLD LABEL (gold chars carrying that label)")
        header = "  ".join(f"{name:>14}" for name in results)
        print(f"  {'label':<28} {'gold chars':>11}  {header}")
        for label in labels:
            # Runs still in flight cover different prefixes of the corpus, so
            # each model has its own denominator; the widest one is shown.
            support = max(
                (r["per_label"].get(label, {}).get("tp", 0)
                 + r["per_label"].get(label, {}).get("fn", 0))
                for r in results.values()
            )
            cells = []
            for r in results.values():
                stat = r["per_label"].get(label, {"tp": 0, "fn": 0})
                total = stat["tp"] + stat["fn"]
                cells.append(
                    f"{stat['tp'] / total:>13.1%} " if total else f"{'-':>14}"
                )
            print(f"  {label:<28} {support:>11,}  {'  '.join(cells)}")
        print()

    if args.by_length:
        print("  CHARACTER RECALL BY GOLD SPAN LENGTH")
        header = "  ".join(f"{name:>14}" for name in results)
        print(f"  {'length':<28} {'gold chars':>11}  {header}")
        for bucket in ["1-4", "5-9", "10-19", "20-39", "40+"]:
            support = max(
                (r["per_length"].get(bucket, {}).get("tp", 0)
                 + r["per_length"].get(bucket, {}).get("fn", 0))
                for r in results.values()
            )
            cells = []
            for r in results.values():
                stat = r["per_length"].get(bucket, {"tp": 0, "fn": 0})
                total = stat["tp"] + stat["fn"]
                cells.append(
                    f"{stat['tp'] / total:>13.1%} " if total else f"{'-':>14}"
                )
            print(f"  {bucket:<28} {support:>11,}  {'  '.join(cells)}")
        print()

    if args.json_out:
        payload = {
            "corpus": str(corpus_dir),
            "rule": args.rule,
            "pii_density": density,
            "models": {
                name: {
                    "char_precision": prf(**r["overall"])[0],
                    "char_recall": prf(**r["overall"])[1],
                    "char_f1": prf(**r["overall"])[2],
                    "macro_char_f1": r["macro_f1"],
                    "cases": r["n"],
                    "complete": r["complete"],
                    "doc": r["doc"],
                    "per_label": r["per_label"],
                    "per_length": r["per_length"],
                }
                for name, r in results.items()
            },
        }
        Path(args.json_out).write_text(
            json.dumps(payload, indent=2), encoding="utf-8"
        )
        print(f"Wrote {args.json_out}")


if __name__ == "__main__":
    main()
