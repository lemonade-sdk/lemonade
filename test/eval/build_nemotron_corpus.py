"""
Build a routing evaluation corpus from nvidia/Nemotron-PII.

Downloads the dataset from HuggingFace, stratified-samples examples,
wraps each document in an OpenAI chat message envelope, and writes:

  <output_dir>/cases.jsonl   — routing evaluation cases
  <output_dir>/stats.json    — corpus statistics

Each case matches the existing conformance schema:
  {
    "name": str,
    "pii_category": str,        # comma-joined entity types, or "none"
    "note": str,
    "request": {
      "model": str,
      "messages": [{"role": "user", "content": str}]
    },
    "decision": {
      "version": "1",
      "route_to": str,          # privacy_model or cloud_model
      "matched_rule": str,      # "pii-detected" or ""
      "default_used": bool,
      "outputs": {}
    }
  }

Usage:
    python test/eval/build_nemotron_corpus.py \\
        --output-dir test/conformance/routing/1/l2_pii_nemotron \\
        --n-pii 2500 \\
        --n-benign 2500 \\
        --privacy-model Qwen3.5-9B-NoThinking \\
        --cloud-model fireworks.kimi-k2p6 \\
        --split test \\
        --seed 42

Requirements:
    pip install datasets huggingface_hub
"""

import argparse
import ast
import json
import random
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--output-dir",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_nemotron"
        ),
        help="Directory to write cases.jsonl and stats.json",
    )
    p.add_argument(
        "--n-pii",
        type=int,
        default=2500,
        help="Number of PII-containing examples to sample",
    )
    p.add_argument(
        "--n-benign",
        type=int,
        default=2500,
        help="Number of benign (no PII) examples to sample",
    )
    p.add_argument(
        "--privacy-model",
        default="Qwen3.5-9B-NoThinking",
        help="Candidate model that PII should route to",
    )
    p.add_argument(
        "--cloud-model",
        default="fireworks.kimi-k2p6",
        help="Default cloud candidate model",
    )
    p.add_argument(
        "--split",
        default="test",
        choices=["train", "test"],
        help="Nemotron-PII split to use (keep 'test' as held-out)",
    )
    p.add_argument("--seed", type=int, default=42, help="Random seed for sampling")
    p.add_argument(
        "--max-chars",
        type=int,
        default=2000,
        help="Truncate document text to this many characters",
    )
    p.add_argument(
        "--streaming",
        action="store_true",
        default=True,
        help="Stream dataset instead of downloading in full (default: True)",
    )
    p.add_argument(
        "--no-streaming",
        dest="streaming",
        action="store_false",
        help="Download full dataset (requires ~10GB disk)",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Text helpers
# ---------------------------------------------------------------------------

_WHITESPACE_RE = re.compile(r"\s+")


def normalize_text(text: str, max_chars: int) -> str:
    text = _WHITESPACE_RE.sub(" ", text).strip()
    if len(text) > max_chars:
        text = text[:max_chars].rsplit(" ", 1)[0] + " …"
    return text


def parse_spans(raw) -> list[dict]:
    """Nemotron-PII's `spans` column round-trips through parquet as a Python
    repr string (single-quoted, e.g. "[{'start': 0, ..., 'label': 'x'}]" or
    "[]" for no entities), not an actual list or valid JSON."""
    if isinstance(raw, list):
        return raw
    if not isinstance(raw, str) or not raw.strip():
        return []
    try:
        parsed = ast.literal_eval(raw)
    except (ValueError, SyntaxError):
        return []
    return parsed if isinstance(parsed, list) else []


def pii_types_from_spans(spans: list) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for span in spans:
        label = span.get("label", "").strip()
        if label and label not in seen:
            seen.add(label)
            result.append(label)
    return sorted(result)


# ---------------------------------------------------------------------------
# Document → prompt envelope
# ---------------------------------------------------------------------------

PROMPT_PREFIXES = [
    "",
    "Can you help me with the following? ",
    "Please review this: ",
    "I need assistance with the text below. ",
    "Here is a document I need help with: ",
    "Take a look at this and give me your thoughts: ",
    "What do you make of this? ",
    "I have a question about this content: ",
]


def wrap_in_message(text: str, rng: random.Random) -> dict:
    prefix = rng.choice(PROMPT_PREFIXES)
    content = prefix + text if prefix else text
    return {
        "model": "user.PII-Regex-Router",
        "messages": [{"role": "user", "content": content}],
    }


# ---------------------------------------------------------------------------
# Corpus builder
# ---------------------------------------------------------------------------


def build_corpus(args: argparse.Namespace) -> None:
    try:
        from datasets import load_dataset
    except ImportError:
        print(
            "ERROR: 'datasets' package not found. Install with: pip install datasets",
            file=sys.stderr,
        )
        sys.exit(1)

    rng = random.Random(args.seed)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(
        f"Loading nvidia/Nemotron-PII ({args.split} split, streaming={args.streaming})..."
    )
    ds = load_dataset(
        "nvidia/Nemotron-PII",
        split=args.split,
        streaming=args.streaming,
        trust_remote_code=False,
    )

    pii_pool: list[dict] = []
    benign_pool: list[dict] = []

    # Streaming: scan until we have enough candidates in both buckets.
    # We over-collect by 4x to allow for dedup / length filtering after sampling.
    target_pii = args.n_pii * 4
    target_benign = args.n_benign * 4
    scanned = 0

    print(f"Scanning dataset (target: {args.n_pii} PII + {args.n_benign} benign)...")
    for row in ds:
        spans = parse_spans(row.get("spans"))
        text = row.get("text", "")
        if not text or not text.strip():
            continue

        has_pii = len(spans) > 0
        if has_pii and len(pii_pool) < target_pii:
            pii_pool.append(row)
        elif not has_pii and len(benign_pool) < target_benign:
            benign_pool.append(row)

        scanned += 1
        if scanned % 10000 == 0:
            print(
                f"  scanned {scanned:,} rows - PII pool: {len(pii_pool):,}, benign pool: {len(benign_pool):,}"
            )

        if len(pii_pool) >= target_pii and len(benign_pool) >= target_benign:
            break

    print(
        f"Scan complete: {scanned:,} rows read, {len(pii_pool):,} PII candidates, {len(benign_pool):,} benign candidates"
    )

    if len(pii_pool) < args.n_pii:
        print(
            f"WARNING: only {len(pii_pool)} PII examples found, requested {args.n_pii}",
            file=sys.stderr,
        )
    if len(benign_pool) < args.n_benign:
        print(
            f"WARNING: only {len(benign_pool)} benign examples found, requested {args.n_benign}",
            file=sys.stderr,
        )

    rng.shuffle(pii_pool)
    rng.shuffle(benign_pool)

    selected_pii = pii_pool[: args.n_pii]
    selected_benign = benign_pool[: args.n_benign]

    # ---------------------------------------------------------------------------
    # Convert to cases
    # ---------------------------------------------------------------------------
    cases: list[dict] = []
    type_counter: Counter = Counter()
    domain_counter: Counter = Counter()

    for i, row in enumerate(selected_pii):
        text = normalize_text(row["text"], args.max_chars)
        spans = parse_spans(row.get("spans"))
        types = pii_types_from_spans(spans)
        domain = row.get("domain", "unknown")
        doc_type = row.get("document_type", "unknown")

        type_counter.update(types)
        domain_counter[domain] += 1

        case = {
            "name": f"nemotron-pii-{i:05d}",
            "pii_category": ",".join(types) if types else "unknown",
            "note": f"Nemotron-PII {args.split} split. domain={domain} doc_type={doc_type} entities={len(spans)}",
            "request": wrap_in_message(text, rng),
            "decision": {
                "version": "1",
                "route_to": args.privacy_model,
                "matched_rule": "pii-detected",
                "default_used": False,
                "outputs": {},
            },
        }
        cases.append(case)

    for i, row in enumerate(selected_benign):
        text = normalize_text(row["text"], args.max_chars)
        domain = row.get("domain", "unknown")
        doc_type = row.get("document_type", "unknown")

        domain_counter[domain] += 1

        case = {
            "name": f"nemotron-benign-{i:05d}",
            "pii_category": "none",
            "note": f"Nemotron-PII {args.split} split, no spans. domain={domain} doc_type={doc_type}",
            "request": wrap_in_message(text, rng),
            "decision": {
                "version": "1",
                "route_to": args.cloud_model,
                "matched_rule": "",
                "default_used": True,
                "outputs": {},
            },
        }
        cases.append(case)

    rng.shuffle(cases)

    # ---------------------------------------------------------------------------
    # Write outputs
    # ---------------------------------------------------------------------------
    cases_path = output_dir / "cases.jsonl"
    with cases_path.open("w", encoding="utf-8") as f:
        for case in cases:
            f.write(json.dumps(case, ensure_ascii=False) + "\n")

    stats = {
        "source": "nvidia/Nemotron-PII",
        "split": args.split,
        "seed": args.seed,
        "n_pii": len(selected_pii),
        "n_benign": len(selected_benign),
        "total": len(cases),
        "privacy_model": args.privacy_model,
        "cloud_model": args.cloud_model,
        "max_chars": args.max_chars,
        "top_pii_types": type_counter.most_common(20),
        "top_domains": domain_counter.most_common(20),
    }
    stats_path = output_dir / "stats.json"
    stats_path.write_text(
        json.dumps(stats, indent=2, ensure_ascii=False), encoding="utf-8"
    )

    print(f"\nWrote {len(cases):,} cases -> {cases_path}")
    print(f"Wrote stats        -> {stats_path}")
    print(f"\nTop PII types:")
    for label, count in type_counter.most_common(10):
        print(f"  {label:<35} {count:>5}")
    print(f"\nTop domains:")
    for domain, count in domain_counter.most_common(10):
        print(f"  {domain:<35} {count:>5}")


if __name__ == "__main__":
    build_corpus(parse_args())
