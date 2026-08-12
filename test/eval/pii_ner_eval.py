"""
PII NER model evaluator.

Benchmarks a standalone HuggingFace token-classification (NER) model against
the same PII/benign snapshot used by pii_routing_eval.py, entirely independent
of the Lemonade router: this model isn't a chat-completion model, so it can't
be registered as a router candidate or classifier. This script runs it
directly via `transformers`, no Lemonade server involved.

For each case, feeds the full document text to the model and checks whether
it predicts at least one non-"O" BIO tag on a real (non-special) token. The
default model (llm-semantic-router/mmbert32k-pii-detector-merged) tags 17
entity types (PERSON, EMAIL_ADDRESS, US_SSN, CREDIT_CARD, ...) but this script
only checks presence/absence of ANY entity, matching pii_routing_eval.py's
has_pii signal - it does not attempt to map this model's 17-type taxonomy
onto Nemotron-PII's ~30+ finer-grained categories (lossy, not what's asked).
Detected types ARE logged per case for manual inspection.

Caveats specific to this model, found by direct inspection before writing
this script (see conversation) - re-verify if you swap --model:
  - The <bos> special token unconditionally predicts B-US_DRIVER_LICENSE
    regardless of content; excluded here via the tokenizer's
    special_tokens_mask, not by string-matching the token.
  - model.config.id2label is int-keyed for this model, NOT str-keyed as the
    model card's own example code assumes (`id2label[str(pred.item())]`
    silently KeyErrors on this checkpoint's config).

Metrics mirror pii_routing_eval.py's framing so results are comparable:
TP/FN over PII cases (recall = detection rate, FN = a miss), TN/FP over
benign cases (precision - same n=1 statistical-power caveat as every other
run in this series applies here too).

Requirements:
    pip install transformers torch

Usage:
    python test/eval/pii_ner_eval.py [--corpus-dir DIR] [--model NAME] [--limit N] [--verbose] [--device DEVICE]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron
    --model          llm-semantic-router/mmbert32k-pii-detector-merged
    --limit          0  (all cases; pass e.g. 20 for a quick smoke test first)
    --device         auto (cuda if available, else cpu)
    --max-length     8192  (model supports up to 32768; truncation=True either way)
    --progress-every 50  (0 disables the heartbeat)
    --log-dir        <corpus-dir>/runs/  (pass --no-log-file to skip the backup)

Examples:
    # Smoke test on 20 cases before committing to the full corpus
    python test/eval/pii_ner_eval.py --limit 20 --verbose

    # Full corpus
    python test/eval/pii_ner_eval.py
"""

import argparse
import json
import sys
import time
from pathlib import Path

# Detected-entity text (case notes too) can carry Unicode the tokenizer's own
# special markers (e.g. U+2581 "▁") that Windows' default cp1252 console
# can't represent, crashing print() mid-run. Force UTF-8 with graceful
# fallback, same fix as pii_routing_eval.py / build_nemotron_corpus.py.
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
        default="llm-semantic-router/mmbert32k-pii-detector-merged",
        help="HuggingFace token-classification model id",
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
        "--max-length",
        type=int,
        default=8192,
        help="Tokenizer truncation length. The default model supports up to "
        "32768 tokens; 8192 comfortably covers Nemotron's document lengths "
        "without paying for the full context on every request.",
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
        "Defaults to <corpus-dir>/runs/. Files are timestamped so runs don't collide.",
    )
    p.add_argument(
        "--no-log-file",
        action="store_true",
        help="Skip writing the run log / JSON summary backup.",
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


def load_model(model_name: str, device: str):
    from transformers import AutoModelForTokenClassification, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(model_name)
    model = AutoModelForTokenClassification.from_pretrained(model_name)
    model.to(device)
    model.eval()
    return tokenizer, model


def detect_entity_types(
    text: str, tokenizer, model, device: str, max_length: int
) -> set[str]:
    """Run the model on text and return the set of entity types (e.g.
    "EMAIL_ADDRESS", stripped of its B-/I- BIO prefix) predicted on any real
    (non-special) token. Special tokens are excluded via the tokenizer's own
    special_tokens_mask, not by guessing which strings are "special" -
    <bos>/<eos>/<pad>/etc vary by tokenizer and this model's <bos> in
    particular always fires a spurious label if left in.
    """
    import torch

    inputs = tokenizer(
        text,
        return_tensors="pt",
        truncation=True,
        max_length=max_length,
        return_special_tokens_mask=True,
    )
    special_mask = inputs.pop("special_tokens_mask")[0]
    inputs = {k: v.to(device) for k, v in inputs.items()}

    with torch.no_grad():
        logits = model(**inputs).logits
    predictions = torch.argmax(logits, dim=2)[0].tolist()

    id2label = model.config.id2label
    types: set[str] = set()
    for pred_id, is_special in zip(predictions, special_mask.tolist()):
        if is_special:
            continue
        label = id2label[pred_id]
        if label == "O":
            continue
        entity_type = label.split("-", 1)[-1] if "-" in label else label
        types.add(entity_type)
    return types


# ---------------------------------------------------------------------------
# Evaluation
# ---------------------------------------------------------------------------


def evaluate(args: argparse.Namespace) -> None:
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
    if args.limit and args.limit > 0:
        cases = cases[: args.limit]

    log_file_handle = None
    log_path: Path | None = None
    json_path: Path | None = None
    if not args.no_log_file:
        log_dir = Path(args.log_dir) if args.log_dir else corpus_dir / "runs"
        log_dir.mkdir(parents=True, exist_ok=True)
        model_slug = args.model.split("/")[-1]
        run_id = f"ner_{model_slug}_{time.strftime('%Y%m%d-%H%M%S')}"
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
        log(f"Corpus : {corpus_dir}")
        log(f"Model  : {args.model}")
        log(f"Device : {device}")
        log(f"Cases  : {len(cases)}")
        log()

        log("Loading model (first run downloads weights from HuggingFace)...")
        tokenizer, model = load_model(args.model, device)
        log("Model loaded.\n")

        counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
        wrong_cases: list[dict] = []

        start_time = time.time()
        for idx, case in enumerate(cases, start=1):
            if args.progress_every and idx % args.progress_every == 0:
                elapsed = time.time() - start_time
                rate = idx / elapsed if elapsed > 0 else 0.0
                log(
                    f"  ... {idx}/{len(cases)} cases processed "
                    f"({elapsed:.0f}s elapsed, {rate:.2f} cases/s)"
                )

            name = case.get("name", "?")
            content = case["request"]["messages"][0]["content"]
            expected_pii = case.get("pii_category", "none")
            expected_has_pii = expected_pii != "none"

            try:
                detected = detect_entity_types(
                    content, tokenizer, model, device, args.max_length
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

            if not passed:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": kind,
                        "expected_pii": expected_pii,
                        "detected_types": detected_str,
                        "note": case.get("note", ""),
                    }
                )
                log(
                    f"  [FAIL][{kind}] {name}: expected_pii={expected_pii} detected={detected_str}"
                )
            elif args.verbose:
                log(f"  [PASS][{kind}] {name}: detected={detected_str}")

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
        log("CONFUSION MATRIX")
        log("=" * 60)
        log(f"  PII detected     (TP, correct) : {counts['TP']:3d} / {total_sensitive}")
        log(f"  PII missed       (FN, miss)    : {counts['FN']:3d} / {total_sensitive}")
        log(f"  benign quiet     (TN, correct) : {counts['TN']:3d} / {total_benign}")
        log(f"  benign flagged   (FP)          : {counts['FP']:3d} / {total_benign}")
        log(f"  errors                          : {counts['ERROR']}")
        log()
        log("METRICS")
        log("-" * 40)
        log(
            f"  Miss rate (FN / sensitive)     : {miss_rate:.1%}  <- primary (lower is better)"
        )
        log(f"  False-positive rate (FP/benign): {false_positive_rate:.1%}")
        log(f"  Precision                      : {precision:.3f}")
        log(f"  Recall                         : {recall:.3f}")
        log(f"  F{beta:.0f} score                    : {fbeta:.3f}")
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
                "n_cases": len(cases),
                "elapsed_seconds": elapsed_seconds,
                "counts": counts,
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
