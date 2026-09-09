"""
perplexity-ai/pplx-pii-masking evaluator.

Benchmarks Perplexity's PII-masking model against the same PII/benign snapshot
used by pii_routing_eval.py / pii_ner_eval.py, entirely independent of the
Lemonade router: this model isn't a chat-completion model, so it can't be
registered as a router candidate or classifier. This script runs it directly
via `transformers`, no Lemonade server involved.

Metrics mirror pii_ner_eval.py's framing so results are comparable: TP/FN over
PII cases (recall = detection rate, FN = a miss -> a leak to cloud), TN/FP over
benign cases.

Why this model needs its own script rather than --model on pii_ner_eval.py:
  - It is NOT an AutoModelForTokenClassification. It's a custom architecture
    (`PiiMaskingModel`, auto_map -> modeling_pii_masking.py) that requires
    trust_remote_code=True and exposes model.predict(text) -> (spans,
    sensitivity), not a plain .logits tensor over id2label.
  - Span decoding is a constrained BIOES Viterbi over 37 tags inside the model,
    not an argmax the caller does. Reimplementing argmax here would bypass the
    decoder the model was trained to be read through.
  - It has TWO heads. The token head yields spans (9 PII categories); a separate
    sensitivity head yields one document-level score. They disagree in the
    normal case - the model card's own example returns three correct spans with
    sensitivity=0.027 - so this script scores the SPAN head as the primary
    has_pii signal (matching every other run in this series) and tallies the
    sensitivity head separately as a secondary, never mixing them.

Taxonomy note: this model tags 9 coarse categories (private_person,
account_number, private_url, private_date, private_address, private_email,
private_phone, other_pii, secret). Like pii_ner_eval.py, this script only
checks presence/absence of ANY span, and does not map those 9 onto
Nemotron-PII's ~30+ finer-grained categories (lossy, not what's asked).
Detected labels ARE logged per case for manual inspection.

Context length: the model's head config caps input at max_seq_len=4096 tokens
(the backbone itself is a 32k-position Qwen3 encoder; 4096 is the fine-tuning
window, and the model card says to chunk longer documents). This script chunks
on token-offset boundaries above the cap rather than truncating, so a long
document can never silently drop its only PII span. On the Nemotron corpus the
cap is not binding - the longest document is ~1.7k tokens - so chunking stays
inert there and this run stays comparable to the 32k/128k-context models.

Requirements:
    pip install transformers torch

Usage:
    python test/eval/pii_pplx_eval.py [--corpus-dir DIR] [--model PATH] [--limit N] [--verbose] [--device DEVICE]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron
    --model          perplexity-ai/pplx-pii-masking
    --limit          0  (all cases; pass e.g. 20 for a quick smoke test first)
    --device         auto (cuda if available, else cpu)
    --max-seq-len    4096  (the model's own cap; longer docs are chunked)
    --chunk-overlap  128  (token overlap between chunks, so a span straddling
                     a boundary is still seen whole by one chunk)
    --sensitivity-threshold 0.5  (secondary head only, never the primary signal)
    --progress-every 50  (0 disables the heartbeat)
    --log-dir        <corpus-dir>/runs/  (pass --no-log-file to skip the backup)
    --resume-from    none (path to a prior run's .log to continue after an
                     interruption - same semantics as pii_ner_eval.py: cases
                     already logged there are skipped, tallies reconstructed
                     from the log line. Requires the prior run used --verbose.)

Examples:
    # Smoke test on 20 cases before committing to the full corpus
    python test/eval/pii_pplx_eval.py --limit 20 --verbose

    # Full 20k corpus, matching the mmBERT / OpenMed / GLiNER runs
    python test/eval/pii_pplx_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k

    # Resume a run that died partway through
    python test/eval/pii_pplx_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --resume-from test/conformance/routing/1/l2_pii_nemotron_20k/runs/pplx_pplx-pii-masking_20260908-181500.log
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

# Detected-span text (case notes too) can carry Unicode that Windows' default
# cp1252 console can't represent, crashing print() mid-run. Force UTF-8 with
# graceful fallback, same fix as pii_ner_eval.py / build_nemotron_corpus.py.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


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
        help="Directory containing cases.jsonl (policy.json is not used - this "
        "script doesn't touch the Lemonade router at all)",
    )
    p.add_argument(
        "--model",
        default="perplexity-ai/pplx-pii-masking",
        help="HuggingFace model id or a local snapshot directory",
    )
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only evaluate the first N cases (0 = all, the default). Cases in "
        "the corpus are pre-shuffled by the builder, so a small N still gives a "
        "random-but-reproducible mix for a smoke test before a full run.",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--device",
        default=None,
        help="cuda / cpu / mps. Defaults to cuda if available, else cpu.",
    )
    p.add_argument(
        "--dtype",
        default=None,
        help="float32 / bfloat16 / float16. Defaults to bfloat16 on cuda (the "
        "checkpoint's stored dtype) and float32 elsewhere, since CPU bfloat16 "
        "matmul is markedly slower than float32 on most x86.",
    )
    p.add_argument(
        "--max-seq-len",
        type=int,
        default=4096,
        help="Model's context cap in tokens. Documents longer than this are "
        "chunked (not truncated). 4096 is this checkpoint's config.max_seq_len.",
    )
    p.add_argument(
        "--chunk-overlap",
        type=int,
        default=128,
        help="Token overlap between consecutive chunks, so a PII span "
        "straddling a chunk boundary is still seen whole by one chunk.",
    )
    p.add_argument(
        "--sensitivity-threshold",
        type=float,
        default=0.5,
        help="Threshold for the model's SECONDARY document-level sensitivity "
        "head. Reported alongside but never used as the primary has_pii signal.",
    )
    p.add_argument(
        "--progress-every",
        type=int,
        default=50,
        help="Log a heartbeat every N cases regardless of --verbose (0 disables).",
    )
    p.add_argument(
        "--log-dir",
        default=None,
        help="Directory to back up the full run output (text log + JSON summary). "
        "Defaults to <corpus-dir>/runs/.",
    )
    p.add_argument(
        "--no-log-file", action="store_true", help="Don't write a backup log file"
    )
    p.add_argument(
        "--resume-from",
        default=None,
        help="Path to a prior run's .log to resume after an interruption.",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------


def resolve_device(requested: str | None) -> str:
    if requested:
        return requested
    import torch

    if torch.cuda.is_available():
        return "cuda"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def resolve_dtype(requested: str | None, device: str):
    import torch

    if requested:
        return getattr(torch, requested)
    return torch.bfloat16 if device == "cuda" else torch.float32


def load_model(model_name: str, device: str, dtype):
    from transformers import AutoModel, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(model_name)
    model = AutoModel.from_pretrained(model_name, trust_remote_code=True, dtype=dtype)
    model.to(device)
    model.eval()
    return tokenizer, model


def split_into_chunks(
    text: str, tokenizer, max_seq_len: int, overlap: int
) -> list[str]:
    """Split text into <=max_seq_len-token pieces on token-offset boundaries.

    Returns [text] unchanged when it already fits, so the common case pays only
    one tokenizer pass and the result is identical to feeding the raw text.
    Chunking (rather than the tokenizer's truncation=True) matters because a
    truncated document can silently lose the only span it had, which would show
    up as a model miss rather than the harness artifact it really is.
    """
    enc = tokenizer(text, return_offsets_mapping=True, add_special_tokens=False)
    ids = enc["input_ids"]
    if len(ids) <= max_seq_len:
        return [text]

    offsets = enc["offset_mapping"]
    step = max(1, max_seq_len - overlap)
    chunks: list[str] = []
    for start in range(0, len(ids), step):
        window = offsets[start : start + max_seq_len]
        if not window:
            break
        chunks.append(text[window[0][0] : window[-1][1]])
        if start + max_seq_len >= len(ids):
            break
    return chunks


def detect_spans(
    text: str, tokenizer, model, max_seq_len: int, overlap: int
) -> tuple[set[str], float]:
    """Run the model on text and return (set of span labels, sensitivity score).

    Both heads are read from the same forward pass via model.predict(), which
    applies the checkpoint's constrained BIOES Viterbi decoder. Over multiple
    chunks, labels union and sensitivity takes the max - a document is as
    sensitive as its most sensitive part.
    """
    import torch

    labels: set[str] = set()
    sensitivity = 0.0
    for chunk in split_into_chunks(text, tokenizer, max_seq_len, overlap):
        if not chunk.strip():
            continue
        with torch.no_grad():
            spans, chunk_sensitivity = model.predict(chunk)
        labels.update(s.label for s in spans)
        sensitivity = max(sensitivity, float(chunk_sensitivity))
    return labels, sensitivity


# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------

_RESUME_LINE_RE = re.compile(
    r"^  \[(?:(FAIL|PASS)\]\[(TP|TN|FP|FN)|ERROR)\] ([^\s:]+)(?:: (?:expected_pii=\S* )?detected=(\S+))?"
)


def parse_resume_log(log_path: Path) -> tuple[dict[str, dict], dict[str, int]]:
    if not log_path.exists():
        print(f"ERROR: resume log not found at {log_path}", file=sys.stderr)
        sys.exit(1)

    processed: dict[str, dict] = {}
    counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _RESUME_LINE_RE.match(line)
        if not m:
            continue
        _, kind, name, detected = m.groups()
        if name in processed:
            continue
        kind = kind or "ERROR"
        processed[name] = {"kind": kind, "detected": detected or "none"}
        counts[kind] += 1
    return processed, counts


# ---------------------------------------------------------------------------
# Evaluate
# ---------------------------------------------------------------------------


def evaluate(args: argparse.Namespace) -> None:
    corpus_dir = Path(args.corpus_dir)
    cases_path = corpus_dir / "cases.jsonl"

    if not cases_path.exists():
        print(f"ERROR: cases.jsonl not found at {cases_path}", file=sys.stderr)
        sys.exit(1)

    all_cases = [
        json.loads(line)
        for line in cases_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if args.limit and args.limit > 0:
        all_cases = all_cases[: args.limit]

    counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
    # Secondary head, tallied independently of the primary span signal.
    sens_counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0}
    wrong_cases: list[dict] = []
    cases = all_cases
    resume_note = ""
    if args.resume_from:
        resume_log_path = Path(args.resume_from)
        processed, counts = parse_resume_log(resume_log_path)
        by_name = {c.get("name", "?"): c for c in all_cases}
        for name, info in processed.items():
            if info["kind"] in ("FN", "FP") and name in by_name:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": info["kind"],
                        "expected_pii": by_name[name].get("pii_category", "none"),
                        "detected_types": info["detected"],
                        "note": by_name[name].get("note", ""),
                    }
                )
        cases = [c for c in all_cases if c.get("name", "?") not in processed]
        resume_note = (
            f" (resuming from {resume_log_path.name}: "
            f"{len(processed)} already done, {len(cases)} remaining)"
        )

    log_file_handle = None
    log_path: Path | None = None
    json_path: Path | None = None
    run_id = ""
    if not args.no_log_file:
        if args.resume_from:
            resume_log_path = Path(args.resume_from)
            log_path = resume_log_path
            json_path = resume_log_path.with_suffix(".json")
            run_id = resume_log_path.stem
            log_file_handle = log_path.open("a", encoding="utf-8", buffering=1)
        else:
            log_dir = Path(args.log_dir) if args.log_dir else corpus_dir / "runs"
            log_dir.mkdir(parents=True, exist_ok=True)
            model_slug = args.model.rstrip("/\\").replace("\\", "/").split("/")[-1]
            run_id = f"pplx_{model_slug}_{time.strftime('%Y%m%d-%H%M%S')}"
            log_path = log_dir / f"{run_id}.log"
            json_path = log_dir / f"{run_id}.json"
            log_file_handle = log_path.open("w", encoding="utf-8", buffering=1)

    def log(msg: str = "") -> None:
        print(msg, flush=True)
        if log_file_handle:
            log_file_handle.write(msg + "\n")
            log_file_handle.flush()

    try:
        device = resolve_device(args.device)
        dtype = resolve_dtype(args.dtype, device)
        log(f"\n--- resuming{resume_note} ---" if args.resume_from else "")
        log(f"Corpus      : {corpus_dir}")
        log(f"Model       : {args.model}")
        log(f"Device      : {device} ({dtype})")
        log(f"Max seq len : {args.max_seq_len} (overlap {args.chunk_overlap})")
        log(f"Cases       : {len(all_cases)}{resume_note}")
        log()

        log("Loading model (first run downloads weights from HuggingFace)...")
        tokenizer, model = load_model(args.model, device, dtype)
        log("Model loaded.\n")

        start_time = time.time()
        for idx, case in enumerate(cases, start=1):
            if args.progress_every and idx % args.progress_every == 0:
                elapsed = time.time() - start_time
                rate = idx / elapsed if elapsed > 0 else 0.0
                remaining = (len(cases) - idx) / rate if rate > 0 else 0.0
                log(
                    f"  ... {idx}/{len(cases)} cases processed "
                    f"({elapsed:.0f}s elapsed, {rate:.2f} cases/s, "
                    f"~{remaining / 60:.0f}m left)"
                )

            name = case.get("name", "?")
            content = case["request"]["messages"][0]["content"]
            expected_pii = case.get("pii_category", "none")
            expected_has_pii = expected_pii != "none"

            try:
                detected, sensitivity = detect_spans(
                    content, tokenizer, model, args.max_seq_len, args.chunk_overlap
                )
            except Exception as exc:
                counts["ERROR"] += 1
                log(f"  [ERROR] {name}: {exc}")
                continue

            detected_has_pii = bool(detected)
            detected_str = ",".join(sorted(detected)) if detected else "none"

            if expected_has_pii and detected_has_pii:
                kind = "TP"
            elif expected_has_pii and not detected_has_pii:
                kind = "FN"
            elif not expected_has_pii and not detected_has_pii:
                kind = "TN"
            else:
                kind = "FP"

            passed = kind in ("TP", "TN")
            counts[kind] += 1

            sens_flag = sensitivity >= args.sensitivity_threshold
            if expected_has_pii:
                sens_counts["TP" if sens_flag else "FN"] += 1
            else:
                sens_counts["FP" if sens_flag else "TN"] += 1

            if not passed:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": kind,
                        "expected_pii": expected_pii,
                        "detected_types": detected_str,
                        "sensitivity": round(sensitivity, 4),
                        "note": case.get("note", ""),
                    }
                )
                log(
                    f"  [FAIL][{kind}] {name}: expected_pii={expected_pii} "
                    f"detected={detected_str} sensitivity={sensitivity:.3f}"
                )
            elif args.verbose:
                log(
                    f"  [PASS][{kind}] {name}: detected={detected_str} "
                    f"sensitivity={sensitivity:.3f}"
                )

        elapsed_seconds = time.time() - start_time
        log(f"\nFinished processing {len(cases)} cases in {elapsed_seconds:.0f}s.\n")

        total_sensitive = counts["TP"] + counts["FN"]
        total_benign = counts["TN"] + counts["FP"]
        recall = counts["TP"] / total_sensitive if total_sensitive else 0.0
        miss_rate = counts["FN"] / total_sensitive if total_sensitive else 0.0
        precision = (
            counts["TP"] / (counts["TP"] + counts["FP"])
            if (counts["TP"] + counts["FP"]) > 0
            else 0.0
        )
        false_positive_rate = counts["FP"] / total_benign if total_benign else 0.0
        beta = 2.0
        fbeta = (
            (1 + beta**2) * precision * recall / (beta**2 * precision + recall)
            if (precision + recall) > 0
            else 0.0
        )

        log("=" * 60)
        log("CONFUSION MATRIX (primary: span head)")
        log("=" * 60)
        log(f"  PII detected     (TP, correct) : {counts['TP']:5d} / {total_sensitive}")
        log(f"  PII missed       (FN, miss)    : {counts['FN']:5d} / {total_sensitive}")
        log(f"  benign quiet     (TN, correct) : {counts['TN']:5d} / {total_benign}")
        log(f"  benign flagged   (FP)          : {counts['FP']:5d} / {total_benign}")
        log(f"  errors                         : {counts['ERROR']}")
        log()
        log("METRICS")
        log("-" * 40)
        log(
            f"  Miss rate (FN / sensitive)     : {miss_rate:.2%}  <- primary (lower is better)"
        )
        log(f"  False-positive rate (FP/benign): {false_positive_rate:.2%}")
        log(f"  Precision                      : {precision:.4f}")
        log(f"  Recall                         : {recall:.4f}")
        log(f"  F{beta:.0f} score                    : {fbeta:.4f}")
        log()

        sens_sensitive = sens_counts["TP"] + sens_counts["FN"]
        sens_recall = sens_counts["TP"] / sens_sensitive if sens_sensitive else 0.0
        log(
            f"SECONDARY: sensitivity head @ threshold {args.sensitivity_threshold} "
            "(NOT the routing signal)"
        )
        log("-" * 40)
        log(f"  would-flag (TP)                : {sens_counts['TP']:5d}")
        log(f"  would-miss (FN)                : {sens_counts['FN']:5d}")
        log(f"  recall                         : {sens_recall:.4f}")
        log()

        if wrong_cases:
            log(f"FAILURES ({len(wrong_cases)})")
            log("-" * 40)
            for wc in wrong_cases[:50]:
                log(f"  [{wc['kind']}] {wc['name']}")
                log(f"    expected_pii={wc['expected_pii']}")
                log(f"    detected    ={wc['detected_types']}")
                if wc["note"]:
                    log(f"    note: {wc['note']}")
            if len(wrong_cases) > 50:
                log(f"  ... and {len(wrong_cases) - 50} more (see JSON summary)")
            log()

        log("=" * 60)
        overall = counts["FN"] == 0 and counts["ERROR"] == 0
        result = "PASS - zero misses" if overall else "FAIL - see above"
        log(f"RESULT: {result}")
        log("=" * 60)

        if json_path:
            summary = {
                "run_id": run_id,
                "corpus_dir": str(corpus_dir),
                "model": args.model,
                "device": device,
                "dtype": str(dtype),
                "max_seq_len": args.max_seq_len,
                "chunk_overlap": args.chunk_overlap,
                "sensitivity_threshold": args.sensitivity_threshold,
                "n_cases": len(all_cases),
                "elapsed_seconds": elapsed_seconds,
                "counts": counts,
                "sensitivity_head_counts": sens_counts,
                "sensitivity_head_recall": sens_recall,
                "recall": recall,
                "miss_rate": miss_rate,
                "precision": precision,
                "false_positive_rate": false_positive_rate,
                "fbeta": fbeta,
                "result": result,
                "wrong_cases": wrong_cases,
            }
            json_path.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8"
            )
            log(f"\nFull log     -> {log_path}")
            log(f"JSON summary -> {json_path}")

        sys.exit(0 if overall else 1)
    finally:
        if log_file_handle:
            log_file_handle.close()


if __name__ == "__main__":
    evaluate(parse_args())
