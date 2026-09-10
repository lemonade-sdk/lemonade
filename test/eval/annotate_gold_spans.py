"""
Back-fill gold PII span offsets onto an existing Nemotron routing corpus.

build_nemotron_corpus.py now preserves span offsets (normalize_text_with_map /
remap_span), but the corpora already built and already benchmarked do not carry
them, and re-running the builder would resample: a fresh corpus is a different
document set, so every document-level number measured against the old one stops
being comparable. This script instead annotates the corpus in place, matching
each case back to its source row by normalized text, so `pii_spans` appears
beside the existing `pii_category` on the same 20,001 cases.

Why matching on text rather than re-deriving from `start`/`end` alone: the raw
offsets index into the raw dataset text, and two edits sit between that and the
prompt a model sees - the whitespace collapse and the random prompt prefix. The
match recovers which raw row a case came from; normalize_text_with_map() then
supplies the index map, and the resolved prefix length supplies the shift.

Substring search is NOT a fallback for either step. Gold strings repeat inside a
document (one case carries `Richard` at 5+ positions), so searching for the span
text answers "is it present" and not "where".

Every remapped span is verified against the prompt it now indexes into:
`content[start:end]` must equal the span's own text after the same whitespace
collapse. Anything that fails is counted and dropped rather than written out.

The comparison is case-insensitive, and that is not a loosened check. The
dataset's `text` field is a canonicalized copy of the entity ("compliance
officer", "male", "kreditexpress.ru") while `start`/`end` point at the true
casing in the document ("Compliance Officer", "Male", "kreditExpress.ru"). On
the 20k corpus 1,418 of 170,974 spans differ that way and **zero** differ any
other way, so a case-sensitive check would discard 1,418 correctly located
spans. The offsets, not the `text` field, are what character scoring consumes.

Requirements:
    pip install pyarrow huggingface_hub

Usage:
    python test/eval/annotate_gold_spans.py [--corpus-dir DIR] [--output FILE]

Defaults:
    --corpus-dir  test/conformance/routing/1/l2_pii_nemotron_20k
    --split       test
    --output      <corpus-dir>/cases.jsonl  (in place; a .bak is written first)

Examples:
    python test/eval/annotate_gold_spans.py \\
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k
"""

import argparse
import json
import shutil
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from build_nemotron_corpus import (  # noqa: E402
    PROMPT_PREFIXES,
    normalize_text,
    normalize_text_with_map,
    parse_spans,
    remap_span,
)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

DATASET = "nvidia/Nemotron-PII"


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
            / "l2_pii_nemotron_20k"
        ),
        help="Directory containing cases.jsonl",
    )
    p.add_argument("--split", default="test", choices=["train", "test"])
    p.add_argument(
        "--output",
        default="",
        help="Where to write the annotated cases (default: in place)",
    )
    p.add_argument(
        "--max-chars",
        type=int,
        default=0,
        help="The --max-chars the corpus was built with (from its stats.json)",
    )
    return p.parse_args()


def load_source_rows(split: str) -> tuple[list[str], list[list]]:
    """(raw texts, parsed spans) for every row of the split.

    Reads the parquet directly rather than through `datasets`: the split is one
    150MB file, and going through it column-wise avoids materializing the rest
    of the schema.
    """
    import pyarrow.parquet as pq
    from huggingface_hub import hf_hub_download

    print(f"Fetching {DATASET} ({split} split)...")
    path = hf_hub_download(
        DATASET,
        f"data/{split}-00000-of-00001.parquet",
        repo_type="dataset",
    )
    print(f"  {path}")
    table = pq.read_table(path, columns=["text", "spans"])
    texts = table.column("text").to_pylist()
    spans = [parse_spans(s) for s in table.column("spans").to_pylist()]
    print(f"  {len(texts):,} rows")
    return texts, spans


def build_index(texts: list[str], max_chars: int) -> tuple[dict[str, int], int]:
    """normalized text -> row index. Ambiguous texts are excluded, not guessed."""
    index: dict[str, int] = {}
    duplicates: set[str] = set()
    for i, raw in enumerate(texts):
        if not raw or not raw.strip():
            continue
        norm = normalize_text(raw, max_chars)
        if norm in index:
            duplicates.add(norm)
        else:
            index[norm] = i
    for norm in duplicates:
        index.pop(norm, None)
    return index, len(duplicates)


def resolve_prefix(content: str, index: dict[str, int]) -> tuple[int, int] | None:
    """(row index, prefix length) for a case's prompt, or None if unresolvable.

    A document can itself begin with the text of one of the prefixes, so every
    prefix that matches is tried and the answer is accepted only when exactly
    one of them lands on a known row.
    """
    hits = []
    for prefix in sorted(set(PROMPT_PREFIXES), key=len, reverse=True):
        if not content.startswith(prefix):
            continue
        row = index.get(content[len(prefix) :])
        if row is not None:
            hits.append((row, len(prefix)))
    if len(hits) == 1:
        return hits[0]
    return None


def annotate(args: argparse.Namespace) -> None:
    corpus_dir = Path(args.corpus_dir)
    cases_path = corpus_dir / "cases.jsonl"
    if not cases_path.exists():
        print(f"ERROR: cases.jsonl not found at {cases_path}", file=sys.stderr)
        sys.exit(1)

    cases = [
        json.loads(line)
        for line in cases_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    print(f"Loaded {len(cases):,} cases from {cases_path}")

    texts, all_spans = load_source_rows(args.split)
    index, n_duplicates = build_index(texts, args.max_chars)
    print(f"Indexed {len(index):,} unique normalized documents "
          f"({n_duplicates:,} ambiguous texts excluded)")

    stats = Counter()
    label_counts: Counter = Counter()
    for case in cases:
        content = case["request"]["messages"][0]["content"]
        expects_pii = case.get("pii_category", "none") != "none"

        resolved = resolve_prefix(content, index)
        if resolved is None:
            case["pii_spans"] = []
            stats["unmatched_cases"] += 1
            if expects_pii:
                stats["unmatched_pii_cases"] += 1
            continue
        row, prefix_len = resolved
        stats["matched_cases"] += 1

        _, orig_to_new = normalize_text_with_map(texts[row], args.max_chars)
        remapped = []
        for span in all_spans[row]:
            if not isinstance(span, dict):
                continue
            stats["source_spans"] += 1
            out = remap_span(span, orig_to_new, prefix_len)
            if out is None:
                stats["dropped_in_remap"] += 1
                continue
            found = content[out["start"] : out["end"]]
            want = normalize_text(out["text"], 0)
            if found != want:
                if found.casefold() != want.casefold():
                    stats["offset_mismatch"] += 1
                    continue
                stats["casefold_only"] += 1
            remapped.append(out)
            label_counts[out["label"]] += 1
        case["pii_spans"] = remapped
        stats["gold_spans"] += len(remapped)
        if expects_pii and not remapped:
            stats["pii_case_without_spans"] += 1
        if remapped:
            stats["cases_with_spans"] += 1
            covered = set()
            for span in remapped:
                covered.update(
                    i
                    for i in range(span["start"], min(span["end"], len(content)))
                    if not content[i].isspace()
                )
            stats["gold_chars"] += len(covered)
        stats["prompt_chars"] += sum(1 for ch in content if not ch.isspace())

    out_path = Path(args.output) if args.output else cases_path
    if out_path == cases_path:
        backup = cases_path.with_suffix(".jsonl.bak")
        if not backup.exists():
            shutil.copy2(cases_path, backup)
            print(f"Backed up original -> {backup}")

    tmp = out_path.with_suffix(".jsonl.tmp")
    with tmp.open("w", encoding="utf-8") as f:
        for case in cases:
            f.write(json.dumps(case, ensure_ascii=False) + "\n")
    tmp.replace(out_path)

    density = (
        stats["gold_chars"] / stats["prompt_chars"] if stats["prompt_chars"] else 0.0
    )
    print()
    print("=" * 60)
    print("GOLD SPAN ANNOTATION")
    print("=" * 60)
    print(f"  cases matched to a source row : {stats['matched_cases']:,} / {len(cases):,}")
    print(f"  cases unmatched               : {stats['unmatched_cases']:,} "
          f"({stats['unmatched_pii_cases']:,} of them PII-labeled)")
    print(f"  gold spans written            : {stats['gold_spans']:,}")
    print(f"  cases carrying >=1 span       : {stats['cases_with_spans']:,}")
    print(f"  spans dropped in remap        : {stats['dropped_in_remap']:,}")
    print(f"  spans matching case-insensitively only : {stats['casefold_only']:,}")
    print(f"  spans failing offset check    : {stats['offset_mismatch']:,}")
    print(f"  PII cases left with no spans  : {stats['pii_case_without_spans']:,}")
    print(f"  gold PII characters (non-ws)  : {stats['gold_chars']:,}")
    print(f"  prompt characters (non-ws)    : {stats['prompt_chars']:,}")
    print(f"  PII density (char-F1 ceiling for a flag-everything model): {density:.4f}")
    print()
    print("  Top gold labels:")
    for label, count in label_counts.most_common(15):
        print(f"    {label:<32} {count:>7,}")
    print()
    print(f"Wrote {out_path}")

    summary = {
        "dataset": DATASET,
        "split": args.split,
        "max_chars": args.max_chars,
        "cases": len(cases),
        "distinct_gold_labels": len(label_counts),
        "pii_density": density,
        **{k: v for k, v in sorted(stats.items())},
        "label_counts": dict(label_counts.most_common()),
    }
    summary_path = corpus_dir / "gold_spans.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"Wrote {summary_path}")


if __name__ == "__main__":
    annotate(parse_args())
