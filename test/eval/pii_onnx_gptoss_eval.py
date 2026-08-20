"""
PII NER model evaluator - ONNX export of OpenMed/privacy-filter-multilingual-v2.

Benchmarks the ONNX export produced by privacy_filter2onnx.py directly via
onnxruntime's CPUExecutionProvider, entirely independent of the Lemonade
router: this model can't be registered as a router candidate/classifier (same
situation pii_ner_eval.py documents for other standalone NER models). So this
mirrors pii_ner_eval.py's standalone-script pattern instead of going through
pii_routing_eval.py.

The exported graph takes a plain 2D input_ids + attention_mask (see
privacy_filter2onnx.py's module docstring for how that was confirmed correct
against the true native model, and the two bugs an earlier, wrong
reinterpretation of this checkpoint had silently gotten past) - no special
mask construction needed on this script's side at all, unlike a first attempt
at this eval that assumed the model needed precomputed 4D masks.

This model's labels are BIOES (B-/I-/E-/S- prefixes, not just BIO), across
217 fine-grained entity types - stripped to the bare entity name the same way
pii_ner_eval.py does (split on first "-"), since this script only checks
presence/absence of ANY entity, matching pii_routing_eval.py's has_pii signal.

Metrics/log format mirror pii_ner_eval.py so results are directly comparable
across the whole model-comparison series.

Requirements:
    pip install onnxruntime torch transformers huggingface_hub
    (run test/eval/privacy_filter2onnx.py first to produce the ONNX export)

Usage:
    python test/eval/pii_onnx_gptoss_eval.py [--corpus-dir DIR] [--model-dir DIR] [--repo-id ID] [--limit N] [--verbose] [--max-length N] [--progress-every N] [--log-dir DIR] [--no-log-file] [--resume-from LOG]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron
    --model-dir      ~/.cache/lemonade-onnx-models/privacy-filter-ml-v2-onnx
                     (model.onnx + model.onnx.data, from privacy_filter2onnx.py)
    --repo-id        OpenMed/privacy-filter-multilingual-v2 (for tokenizer.json
                     and config.json's id2label only - cached, not the 2.8GB
                     safetensors weights, which this script never touches)
    --limit          0 (all cases; pass e.g. 20 for a smoke test first)
    --max-length     4096
    --progress-every 50 (0 disables the heartbeat)
    --log-dir        <corpus-dir>/runs/ (pass --no-log-file to skip the backup)
    --resume-from    none (path to a prior --verbose run's .log to continue
                     after an interruption - same mechanics as pii_ner_eval.py)

Examples:
    # Smoke test on 20 cases before committing to the full corpus
    python test/eval/pii_onnx_gptoss_eval.py --limit 20 --verbose

    # Full 20k corpus
    python test/eval/pii_onnx_gptoss_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

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
        "--model-dir",
        default=os.path.join(
            os.path.expanduser("~"),
            ".cache",
            "lemonade-onnx-models",
            "privacy-filter-ml-v2-onnx",
        ),
        help="Directory containing model.onnx (+ model.onnx.data alongside it), "
        "produced by test/eval/privacy_filter2onnx.py",
    )
    p.add_argument(
        "--repo-id",
        default="OpenMed/privacy-filter-multilingual-v2",
        help="HF repo id to pull tokenizer.json/config.json's id2label from "
        "(small, cached already if privacy_filter2onnx.py has been run) - the "
        "~2.8GB safetensors weights are never touched by this script.",
    )
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only evaluate the first N cases (0 = all, the default).",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--max-length",
        type=int,
        default=4096,
        help="Tokenizer truncation length.",
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
    p.add_argument(
        "--resume-from",
        default=None,
        help="Path to a prior (--verbose) run's .log to continue after an "
        "interruption, instead of starting over.",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------


def load_model(model_dir: str, repo_id: str):
    import onnxruntime as ort
    from huggingface_hub import hf_hub_download
    from transformers import AutoTokenizer

    onnx_path = os.path.join(model_dir, "model.onnx")
    if not os.path.exists(onnx_path):
        print(
            f"ERROR: {onnx_path} not found - run test/eval/privacy_filter2onnx.py first",
            file=sys.stderr,
        )
        sys.exit(1)

    tokenizer = AutoTokenizer.from_pretrained(repo_id)
    config_path = hf_hub_download(repo_id=repo_id, filename="config.json")
    with open(config_path, "r", encoding="utf-8") as f:
        raw_config = json.load(f)
    id2label = {int(k): v for k, v in raw_config["id2label"].items()}
    session = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    return session, tokenizer, id2label


def detect_entity_types(
    text: str, session, tokenizer, id2label: dict, max_length: int
) -> set[str]:
    """Run the ONNX model on text and return the set of entity types (e.g.
    "EMAIL", stripped of its B-/I-/E-/S- BIOES prefix) predicted on any real
    (non-special) token. Special tokens are excluded via the tokenizer's own
    special_tokens_mask, same approach pii_ner_eval.py uses.
    """
    encoded = tokenizer(
        text,
        return_tensors="np",
        truncation=True,
        max_length=max_length,
        return_special_tokens_mask=True,
    )
    special_mask = encoded.pop("special_tokens_mask")[0]

    (logits,) = session.run(
        None,
        {
            "input_ids": encoded["input_ids"],
            "attention_mask": encoded["attention_mask"],
        },
    )
    predictions = logits[0].argmax(axis=-1).tolist()

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
# Resume
# ---------------------------------------------------------------------------

_RESUME_LINE_RE = re.compile(
    r"^  \[(?:(FAIL|PASS)\]\[(TP|TN|FP|FN)|ERROR)\] ([^\s:]+)(?:: (?:expected_pii=\S* )?detected=(\S+))?"
)


def parse_resume_log(log_path: Path) -> tuple[dict[str, dict], dict[str, int]]:
    """Reconstruct per-case outcomes from a prior --verbose run's log, so
    those cases can be skipped (not re-inferred) on resume. Same mechanics as
    pii_ner_eval.py's parse_resume_log - ERROR'd cases are intentionally NOT
    included, so they get retried rather than permanently skipped.
    """
    processed: dict[str, dict] = {}
    counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
    for line in log_path.read_text(encoding="utf-8").splitlines():
        m = _RESUME_LINE_RE.match(line)
        if not m:
            continue
        status, kind, name, detected = m.groups()
        if status is None:
            counts["ERROR"] += 1
            continue
        processed[name] = {"kind": kind, "detected": detected or "none"}
        counts[kind] += 1
    return processed, counts


# ---------------------------------------------------------------------------
# Evaluation
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
            run_id = f"onnx_privacy-filter-ml-v2_{time.strftime('%Y%m%d-%H%M%S')}"
            log_path = log_dir / f"{run_id}.log"
            json_path = log_dir / f"{run_id}.json"
            log_file_handle = log_path.open("w", encoding="utf-8", buffering=1)

    def log(msg: str = "") -> None:
        print(msg, flush=True)
        if log_file_handle:
            log_file_handle.write(msg + "\n")
            log_file_handle.flush()

    try:
        log(f"\n--- resuming{resume_note} ---" if args.resume_from else "")
        log(f"Corpus    : {corpus_dir}")
        log(f"Model dir : {args.model_dir}")
        log(f"Cases     : {len(all_cases)}{resume_note}")
        log()

        log("Loading ONNX session + tokenizer + labels ...")
        session, tokenizer, id2label = load_model(args.model_dir, args.repo_id)
        log("Model loaded.\n")

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
                    content, session, tokenizer, id2label, args.max_length
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
                "model_dir": args.model_dir,
                "n_cases": len(all_cases),
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
