"""
Build a benign (PII-free) routing corpus from real user prompts.

Nemotron-PII is pure-positive - a scan of 30,000 test rows found no document
without PII - so every corpus built from it alone has an empty benign arm and
the eval's over-route rate is 0/0. A router that sends everything to the local
model scores a perfect 0% leak rate on such a corpus (Qwen3.5-0.8B does exactly
this with the blog prompt). This script builds the missing negative arm from
human-written instruction datasets and writes the same case schema as
build_nemotron_corpus.py, so pii_routing_eval.py scores it unchanged.

Sources (Tier A, "ordinary requests that belong on the cloud"):

  HuggingFaceH4/no_robots        first user turn; Generation, Open QA,
                                 Brainstorm, Chat, Rewrite, Classify, Coding
  databricks/databricks-dolly-15k  instruction (+context); open_qa, general_qa,
                                 brainstorming, creative_writing, classification

Categories that paste a document into the prompt (Summarize, Closed QA,
Extract, summarization, closed_qa, information_extraction) are excluded up
front - those documents are Wikipedia/news text full of names and places.

"Benign" is scored against the ROUTER PROMPT's definition of PII, not a
classic-identifier one. The blog prompt lists dates, times, cities, states,
countries, company names, occupations and URLs alongside SSNs and emails, so
a prompt mentioning "Paris" or "in 1969" is not a valid negative: the router
is *correct* to keep it local. Every candidate therefore has to survive three
filters, and the funnel counts in stats.json show what each one removed:

  1. regex   high-precision patterns for emails, URLs/domains, IPs, MACs,
             phone numbers, SSNs, card numbers, @handles, dates (numeric, ISO,
             month-name, bare years), clock times, long digit runs, key-like
             tokens.
  2. OpenMed privacy-filter-multilingual-v2 (ONNX, 54 ai4privacy-style
             types incl. ORGANIZATION, CITY, STATE, DATE, JOBTITLE, URL).
             Its currency/amount/direction labels are not PII and are ignored.
  3. mmBERT32k PII detector (17 Presidio-style types) - adds GPE (countries)
             and NRP (nationality / religion / politics), which OpenMed lacks.

A candidate is kept only if all three are silent. This is deliberately
strict: false rejections cost nothing (the source pools are large), a false
acceptance poisons the arm.

The result is a benign-only corpus, or - with --mix-pii-corpus - a shuffled
mix of N PII cases taken from an existing (already route_to-rewritten) corpus
and the benign cases, so one eval run yields leak rate, over-route rate and
F-beta together.

Usage:
    python test/eval/build_benign_corpus.py --output-dir <dir> --n-benign 500 \\
        --cloud-model fireworks.kimi-k2p6 --privacy-model Qwen3.5-0.8B-GGUF

    # 500 PII + 500 benign, one run dir
    python test/eval/build_benign_corpus.py --output-dir <dir> --n-benign 500 \\
        --mix-pii-corpus <run_dir>/cases.jsonl --n-pii 500 \\
        --cloud-model fireworks.kimi-k2p6 --privacy-model Qwen3.5-0.8B-GGUF

    # same benign set, different router model (no detector pass)
    python test/eval/build_benign_corpus.py --output-dir <dir2> --n-benign 500 \\r
        --reuse-benign <dir>/cases.jsonl \\r
        --mix-pii-corpus <run_dir2>/cases.jsonl --n-pii 500 \\r
        --cloud-model fireworks.kimi-k2p6 --privacy-model Qwen3.5-2B-GGUF

Requirements:
    pip install datasets huggingface_hub transformers torch onnxruntime
"""

import argparse
import json
import random
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_nemotron_corpus import wrap_in_message  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

OPENMED_REPO = "lemonade-sdk/openmed-privacy-filter-multilingual-v2-onnx"
MMBERT_REPO = "llm-semantic-router/mmbert32k-pii-detector-merged"

# Labels OpenMed emits that the router prompt does not treat as PII.
OPENMED_IGNORE = {
    "AMOUNT",
    "CURRENCY",
    "CURRENCYCODE",
    "CURRENCYNAME",
    "CURRENCYSYMBOL",
    "ORDINALDIRECTION",
}

NO_ROBOTS_CATEGORIES = {
    "Generation",
    "Open QA",
    "Brainstorm",
    "Chat",
    "Rewrite",
    "Classify",
    "Coding",
}
DOLLY_CATEGORIES = {
    "open_qa",
    "general_qa",
    "brainstorming",
    "creative_writing",
    "classification",
}

_MONTH = (
    r"(?:jan|feb|mar|apr|may|jun|jul|aug|sep|sept|oct|nov|dec)"
    r"(?:uary|ruary|ch|il|e|y|ust|tember|ober|ember)?\.?"
)
REGEX_FILTERS = {
    "email": re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+"),
    "url": re.compile(r"(?i)\b(?:https?://|www\.)\S+"),
    "domain": re.compile(
        r"(?i)\b[\w-]+\.(?:com|org|net|io|edu|gov|co|us|uk|de|fr|ai|dev|app)\b"
    ),
    "ipv4": re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b"),
    "ipv6": re.compile(r"\b(?:[0-9a-f]{1,4}:){2,7}[0-9a-f]{1,4}\b", re.I),
    "mac": re.compile(r"\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b", re.I),
    "phone": re.compile(r"(?:\+?\d[\d\-\s().]{7,}\d)"),
    "ssn": re.compile(r"\b\d{3}-\d{2}-\d{4}\b"),
    "card": re.compile(r"\b(?:\d[ -]?){13,19}\b"),
    "handle": re.compile(r"(?<![\w.])@[A-Za-z0-9_]{3,}"),
    "date_numeric": re.compile(r"\b\d{1,2}[/.-]\d{1,2}[/.-]\d{2,4}\b"),
    "date_iso": re.compile(r"\b\d{4}-\d{2}-\d{2}\b"),
    "date_month": re.compile(
        rf"(?i)\b{_MONTH}\s+\d{{1,2}}(?:st|nd|rd|th)?(?:,?\s+\d{{4}})?\b"
        rf"|\b\d{{1,2}}(?:st|nd|rd|th)?\s+(?:of\s+)?{_MONTH}\b"
    ),
    "year": re.compile(r"\b(?:1[5-9]|20)\d{2}s?\b"),
    "time": re.compile(r"\b\d{1,2}:\d{2}(?::\d{2})?\s*(?:[ap]\.?m\.?)?\b", re.I),
    "digit_run": re.compile(r"\d{6,}"),
    "key_like": re.compile(
        r"\b(?=[A-Za-z0-9_\-]*\d)(?=[A-Za-z0-9_\-]*[A-Za-z])[A-Za-z0-9_\-]{24,}\b"
    ),
}


def regex_hits(text: str) -> list[str]:
    return [name for name, rx in REGEX_FILTERS.items() if rx.search(text)]


# ---------------------------------------------------------------------------
# Detectors
# ---------------------------------------------------------------------------


class OpenMedOnnx:
    name = "openmed"

    def __init__(self, max_length: int):
        import onnxruntime as ort
        from huggingface_hub import snapshot_download
        from transformers import AutoTokenizer

        root = Path(
            snapshot_download(
                OPENMED_REPO, allow_patterns=["*.json", "*.onnx", "*.onnx.data"]
            )
        )
        self.tokenizer = AutoTokenizer.from_pretrained(str(root))
        self.session = ort.InferenceSession(
            str(root / "model.onnx"), providers=["CPUExecutionProvider"]
        )
        cfg = json.loads((root / "config.json").read_text(encoding="utf-8"))
        self.id2label = {int(k): v for k, v in cfg["id2label"].items()}
        self.max_length = max_length

    def entity_types(self, text: str) -> set[str]:
        import numpy as np

        enc = self.tokenizer(
            text,
            truncation=True,
            max_length=self.max_length,
            return_special_tokens_mask=True,
            return_tensors="np",
        )
        special = enc.pop("special_tokens_mask")[0]
        feeds = {
            "input_ids": enc["input_ids"].astype(np.int64),
            "attention_mask": enc["attention_mask"].astype(np.int64),
        }
        logits = self.session.run(["logits"], feeds)[0][0]
        preds = logits.argmax(axis=-1)
        types: set[str] = set()
        for pred, is_special in zip(preds.tolist(), special.tolist()):
            if is_special:
                continue
            label = self.id2label[pred]
            if label == "O":
                continue
            entity = label.split("-", 1)[-1]
            if entity not in OPENMED_IGNORE:
                types.add(entity)
        return types


class MmBertTorch:
    name = "mmbert"

    def __init__(self, max_length: int):
        from transformers import AutoModelForTokenClassification, AutoTokenizer

        self.tokenizer = AutoTokenizer.from_pretrained(MMBERT_REPO)
        self.model = AutoModelForTokenClassification.from_pretrained(MMBERT_REPO)
        self.model.eval()
        self.max_length = max_length

    def entity_types(self, text: str) -> set[str]:
        import torch

        enc = self.tokenizer(
            text,
            truncation=True,
            max_length=self.max_length,
            return_special_tokens_mask=True,
            return_tensors="pt",
        )
        special = enc.pop("special_tokens_mask")[0]
        with torch.no_grad():
            preds = self.model(**enc).logits.argmax(dim=-1)[0]
        id2label = self.model.config.id2label
        types: set[str] = set()
        for pred, is_special in zip(preds.tolist(), special.tolist()):
            if is_special:
                continue
            label = id2label[pred]
            if label != "O":
                types.add(label.split("-", 1)[-1])
        return types


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------


def load_candidates(rng: random.Random) -> list[dict]:
    from datasets import load_dataset

    out: list[dict] = []
    nr = load_dataset("HuggingFaceH4/no_robots", split="train")
    for row in nr:
        if row["category"] not in NO_ROBOTS_CATEGORIES:
            continue
        user_turns = [m["content"] for m in row["messages"] if m["role"] == "user"]
        if not user_turns:
            continue
        out.append(
            {
                "source": "no_robots",
                "source_id": row["prompt_id"],
                "category": row["category"],
                "text": user_turns[0].strip(),
            }
        )
    dl = load_dataset("databricks/databricks-dolly-15k", split="train")
    for i, row in enumerate(dl):
        if row["category"] not in DOLLY_CATEGORIES:
            continue
        text = row["instruction"].strip()
        if row["context"].strip():
            text = f"{text}\n\n{row['context'].strip()}"
        out.append(
            {
                "source": "dolly",
                "source_id": str(i),
                "category": row["category"],
                "text": text,
            }
        )
    rng.shuffle(out)
    return out


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--output-dir", required=True)
    p.add_argument("--n-benign", type=int, default=500)
    p.add_argument("--min-chars", type=int, default=80)
    p.add_argument("--max-chars", type=int, default=6000)
    p.add_argument("--privacy-model", default="Qwen3.5-0.8B-GGUF")
    p.add_argument("--cloud-model", default="fireworks.kimi-k2p6")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--max-length", type=int, default=512, help="Detector token cap")
    p.add_argument(
        "--mix-pii-corpus",
        type=Path,
        default=None,
        help="cases.jsonl of an existing PII corpus (route_to already rewritten "
        "for --privacy-model) to shuffle in alongside the benign cases",
    )
    p.add_argument("--n-pii", type=int, default=500)
    p.add_argument(
        "--reuse-benign",
        type=Path,
        default=None,
        help="cases.jsonl of a corpus this script already built; its benign "
        "cases are copied verbatim instead of re-scanning the sources, so every "
        "router model is scored on the identical negative set",
    )
    return p.parse_args()


def scan_benign(args: argparse.Namespace, rng: random.Random) -> tuple[list, dict]:
    print("Loading source datasets...")
    candidates = load_candidates(rng)
    funnel = Counter(loaded=len(candidates))
    print(f"  {len(candidates):,} candidates after category filter")

    print("Loading detectors (OpenMed ONNX + mmBERT)...")
    detectors = [OpenMedOnnx(args.max_length), MmBertTorch(args.max_length)]

    selected: list[dict] = []
    rejected_by: Counter = Counter()
    reject_labels: dict[str, Counter] = {"regex": Counter()}
    for d in detectors:
        reject_labels[d.name] = Counter()

    for cand in candidates:
        if len(selected) >= args.n_benign:
            break
        text = cand["text"]
        if not (args.min_chars <= len(text) <= args.max_chars):
            rejected_by["length"] += 1
            continue
        funnel["length_ok"] += 1

        hits = regex_hits(text)
        if hits:
            rejected_by["regex"] += 1
            reject_labels["regex"].update(hits)
            continue
        funnel["regex_ok"] += 1

        clean = True
        for d in detectors:
            types = d.entity_types(text)
            if types:
                rejected_by[d.name] += 1
                reject_labels[d.name].update(sorted(types))
                clean = False
                break
            funnel[f"{d.name}_ok"] += 1
        if not clean:
            continue

        selected.append(cand)
        if len(selected) % 50 == 0:
            scanned = sum(rejected_by.values()) + len(selected)
            print(
                f"  {len(selected)}/{args.n_benign} benign kept ({scanned:,} scanned)"
            )

    if len(selected) < args.n_benign:
        print(
            f"WARNING: only {len(selected)} benign candidates survived, "
            f"requested {args.n_benign}",
            file=sys.stderr,
        )

    cases: list[dict] = []
    for i, cand in enumerate(selected):
        request, _ = wrap_in_message(cand["text"], rng)
        cases.append(
            {
                "name": f"benign-{cand['source']}-{i:05d}",
                "pii_category": "none",
                "pii_spans": [],
                "note": (
                    f"Tier-A benign. source={cand['source']} "
                    f"category={cand['category']} id={cand['source_id']}"
                ),
                "request": request,
                "decision": {
                    "version": "1",
                    "route_to": args.cloud_model,
                    "matched_rule": "",
                    "default_used": True,
                    "outputs": {},
                },
            }
        )
    scan_stats = {
        "funnel": dict(funnel),
        "rejected_by": dict(rejected_by),
        "reject_labels": {k: dict(v.most_common()) for k, v in reject_labels.items()},
    }
    return cases, scan_stats


def reuse_benign(path: Path, cloud_model: str, n_benign: int) -> tuple[list, dict]:
    cases: list[dict] = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            case = json.loads(line)
            if case.get("pii_category", "none") != "none":
                continue
            case["decision"]["route_to"] = cloud_model
            cases.append(case)
    cases.sort(key=lambda c: c["name"])
    if len(cases) < n_benign:
        print(
            f"WARNING: {path} holds only {len(cases)} benign cases, "
            f"requested {n_benign}",
            file=sys.stderr,
        )
    return cases[:n_benign], {"reused_from": str(path)}


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.reuse_benign:
        cases, scan_stats = reuse_benign(
            args.reuse_benign, args.cloud_model, args.n_benign
        )
    else:
        cases, scan_stats = scan_benign(args, rng)
    benign = list(cases)

    n_pii = 0
    if args.mix_pii_corpus:
        pii_cases = []
        with args.mix_pii_corpus.open(encoding="utf-8") as f:
            for line in f:
                if len(pii_cases) >= args.n_pii:
                    break
                pii_cases.append(json.loads(line))
        targets = Counter(c["decision"]["route_to"] for c in pii_cases)
        if targets and targets.most_common(1)[0][0] != args.privacy_model:
            print(
                f"ERROR: {args.mix_pii_corpus} routes PII to {dict(targets)}, "
                f"not --privacy-model {args.privacy_model}; run "
                "prepare_llm_router_run.py first",
                file=sys.stderr,
            )
            return 1
        n_pii = len(pii_cases)
        cases.extend(pii_cases)
        rng.shuffle(cases)

    with (out_dir / "cases.jsonl").open("w", encoding="utf-8") as f:
        for case in cases:
            f.write(json.dumps(case, ensure_ascii=False) + "\n")

    def note_field(case: dict, key: str) -> str:
        m = re.search(rf"{key}=(\S+)", case["note"])
        return m.group(1) if m else "?"

    lengths = sorted(len(c["request"]["messages"][0]["content"]) for c in benign)
    stats = {
        "sources": {
            "no_robots": sorted(NO_ROBOTS_CATEGORIES),
            "dolly": sorted(DOLLY_CATEGORIES),
        },
        "seed": args.seed,
        "n_benign": len(benign),
        "n_pii": n_pii,
        "pii_corpus": str(args.mix_pii_corpus) if args.mix_pii_corpus else None,
        "total": len(cases),
        "privacy_model": args.privacy_model,
        "cloud_model": args.cloud_model,
        "min_chars": args.min_chars,
        "max_chars": args.max_chars,
        **scan_stats,
        "benign_by_source": dict(Counter(note_field(c, "source") for c in benign)),
        "benign_by_category": dict(
            Counter(
                f"{note_field(c, 'source')}/{note_field(c, 'category')}" for c in benign
            ).most_common()
        ),
        "benign_chars_incl_prefix": {
            "min": lengths[0] if lengths else 0,
            "median": lengths[len(lengths) // 2] if lengths else 0,
            "max": lengths[-1] if lengths else 0,
        },
    }
    (out_dir / "stats.json").write_text(
        json.dumps(stats, indent=2) + "\n", encoding="utf-8"
    )

    print()
    print(f"Wrote {len(cases)} cases -> {out_dir / 'cases.jsonl'}")
    print(f"  benign : {len(benign)}   pii : {n_pii}")
    if "rejected_by" in scan_stats:
        print(f"  rejected: {scan_stats['rejected_by']}")
    print(f"  benign chars min/median/max: {stats['benign_chars_incl_prefix']}")
    print(f"  by category: {stats['benign_by_category']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
