"""
GLiNER PII model evaluator.

GLiNER models (nvidia/gliner-PII and similar) use a fundamentally different
API than the fixed-taxonomy token-classification models pii_ner_eval.py
targets: labels are passed at INFERENCE time (zero-shot span extraction) via
the `gliner` package's model.predict_entities(text, labels, threshold), not
baked into the model's output head as a fixed BIO tag set. That API
difference is why this is a separate script rather than a --model flag on
pii_ner_eval.py.

Because labels are supplied per-call, this script extracts the corpus's own
Nemotron-PII category vocabulary (all distinct pii_category values actually
present in cases.jsonl) and passes that full set as the label list on every
case - not a hand-picked subset. This is a materially different, more
apples-to-apples comparison than pii_ner_eval.py's models, whose fixed
17-55-type taxonomies mostly don't line up with Nemotron's own category
names at all.

Two metrics are reported per case:
  - has_pii detection (TP/FN/TN/FP), same framing as pii_ner_eval.py so the
    headline recall/miss-rate numbers are directly comparable across models.
  - type_coverage: of the specific categories a PII case is labeled with
    (e.g. "email,ssn,first_name"), the fraction GLiNER's detected label set
    actually contains. This is the bonus signal only a zero-shot model with
    matching label vocabulary can give - not computable for the fixed-
    taxonomy models already benchmarked.

Chunking: nvidia/gliner-PII silently truncates to model.config.max_len=384
*words* (per model.data_processor.words_splitter, not subword tokens -
confirmed by direct inspection, not assumed from the warning text). ~17% of
this corpus's documents exceed that. Long documents are split into
overlapping word-count windows (aligned to the same word splitter GLiNER uses
internally, so chunk boundaries match its own tokenization), each window is
run separately, and detected types are unioned across windows for that
document. The overlap exists so an entity whose words straddle a window
boundary still lands fully inside at least one window rather than being
split and possibly missed in both halves.

Requirements:
    pip install gliner

Usage:
    python test/eval/pii_gliner_eval.py [--corpus-dir DIR] [--model NAME] [--limit N] [--verbose] [--threshold FLOAT] [--chunk-words N] [--chunk-overlap N]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron
    --model          nvidia/gliner-PII
    --limit          0  (all cases; pass e.g. 20 for a quick smoke test first)
    --threshold      0.5
    --chunk-words    250  (conservatively below the 384-word limit; a word can
                     tokenize to >1 subword token, so this isn't a 1:1 margin)
    --chunk-overlap  50   (words shared between consecutive windows)
    --progress-every 50  (0 disables the heartbeat)
    --log-dir    <corpus-dir>/runs/  (pass --no-log-file to skip the backup)
    --resume-from none (path to a prior run's .log to continue after an
                 interruption - see pii_ner_eval.py's --resume-from docs for
                 the mechanics, identical here)

Examples:
    # Smoke test on 20 cases before committing to the full corpus
    python test/eval/pii_gliner_eval.py --limit 20 --verbose

    # Full corpus
    python test/eval/pii_gliner_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k

    # Resume a run that died partway through (e.g. machine went to sleep)
    python test/eval/pii_gliner_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --resume-from test/conformance/routing/1/l2_pii_nemotron_20k/runs/gliner_gliner-PII_20260811-211156.log
"""

import argparse
import json
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
        help="Directory containing cases.jsonl (no policy.json used here - "
        "this doesn't touch the Lemonade router at all)",
    )
    p.add_argument("--model", default="nvidia/gliner-PII", help="GLiNER model id")
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only evaluate the first N cases (0 = all, the default).",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--threshold",
        type=float,
        default=0.5,
        help="GLiNER confidence threshold for predict_entities",
    )
    p.add_argument(
        "--chunk-words",
        type=int,
        default=250,
        help="Split documents into windows of at most this many words (per "
        "GLiNER's own word splitter) before running inference, since the "
        "model silently truncates at 384 words. Set >= a very large number "
        "(e.g. 100000) to disable chunking and reproduce the old truncating "
        "behavior.",
    )
    p.add_argument(
        "--chunk-overlap",
        type=int,
        default=50,
        help="Words of overlap between consecutive chunks, so an entity "
        "whose words straddle a boundary still lands fully inside at least "
        "one chunk.",
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
        help="Directory to back up the full run output. Defaults to <corpus-dir>/runs/.",
    )
    p.add_argument(
        "--no-log-file",
        action="store_true",
        help="Skip writing the run log / JSON backup.",
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


def load_model(model_name: str):
    from gliner import GLiNER

    return GLiNER.from_pretrained(model_name)


def chunk_text(text: str, model, chunk_words: int, overlap_words: int) -> list[str]:
    """Split text into overlapping windows using GLiNER's own word splitter,
    so window boundaries line up with what the model actually tokenizes on
    (words, not characters or sentences). Returns [text] unchanged (a single
    "chunk") when it already fits, so short documents take the exact same
    code path as before - no behavior change for the ~83% of the corpus that
    was never truncated in the first place.
    """
    words = list(model.data_processor.words_splitter(text))
    if len(words) <= chunk_words:
        return [text]

    step = max(chunk_words - overlap_words, 1)
    chunks = []
    for start in range(0, len(words), step):
        window = words[start : start + chunk_words]
        if not window:
            break
        char_start = window[0][1]
        char_end = window[-1][2]
        chunks.append(text[char_start:char_end])
        if start + chunk_words >= len(words):
            break
    return chunks


def detect_entity_types(
    text: str,
    model,
    labels: list[str],
    threshold: float,
    chunk_words: int,
    overlap_words: int,
) -> set[str]:
    types: set[str] = set()
    for chunk in chunk_text(text, model, chunk_words, overlap_words):
        entities = model.predict_entities(chunk, labels, threshold=threshold)
        types.update(e["label"] for e in entities)
    return types


def extract_label_vocabulary(cases: list[dict]) -> list[str]:
    labels: set[str] = set()
    for case in cases:
        cat = case.get("pii_category", "none")
        if cat and cat != "none":
            labels.update(cat.split(","))
    return sorted(labels)


# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------

import re

_RESUME_LINE_RE = re.compile(
    r"^  \[(?:(FAIL|PASS)\]\[(TP|TN|FP|FN)|ERROR)\] ([^\s:]+)"
    r"(?:: (?:expected_pii=\S* )?detected=(\S+))?"
    r"(?: type_coverage=(\d+)%)?"
)


def parse_resume_log(
    log_path: Path,
) -> tuple[dict[str, dict], dict[str, int], float, int]:
    """Same mechanics as pii_ner_eval.py's parser, plus reconstructing the
    type_coverage stats from each line's trailing `type_coverage=NN%` (only
    present for PII cases, same as at record time - see the caller). Returns
    (processed, counts, type_coverage_sum, type_coverage_n).
    """
    processed: dict[str, dict] = {}
    counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
    type_coverage_sum = 0.0
    type_coverage_n = 0
    for line in log_path.read_text(encoding="utf-8").splitlines():
        m = _RESUME_LINE_RE.match(line)
        if not m:
            continue
        status, kind, name, detected, coverage_pct = m.groups()
        if status is None:
            counts["ERROR"] += 1
            continue
        processed[name] = {"kind": kind, "detected": detected or "none"}
        counts[kind] += 1
        if coverage_pct is not None:
            type_coverage_sum += int(coverage_pct) / 100
            type_coverage_n += 1
    return processed, counts, type_coverage_sum, type_coverage_n


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
    # Label vocabulary is drawn from the FULL corpus regardless of --limit, so
    # a smoke test still exercises the same label set the full run would use.
    label_vocab = extract_label_vocabulary(all_cases)
    all_cases = all_cases[: args.limit] if args.limit and args.limit > 0 else all_cases

    counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0, "ERROR": 0}
    wrong_cases: list[dict] = []
    type_coverage_sum = 0.0
    type_coverage_n = 0
    cases = all_cases
    resume_note = ""
    if args.resume_from:
        resume_log_path = Path(args.resume_from)
        processed, counts, type_coverage_sum, type_coverage_n = parse_resume_log(
            resume_log_path
        )
        by_name = {c.get("name", "?"): c for c in all_cases}
        for name, info in processed.items():
            if info["kind"] in ("FN", "FP") and name in by_name:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": info["kind"],
                        "expected_pii": by_name[name].get("pii_category", "none"),
                        "detected_types": info["detected"],
                        "type_coverage": None,
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
            model_slug = args.model.split("/")[-1]
            run_id = f"gliner_{model_slug}_{time.strftime('%Y%m%d-%H%M%S')}"
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
        log(f"Model     : {args.model}")
        log(f"Cases     : {len(all_cases)}{resume_note}")
        log(f"Threshold : {args.threshold}")
        log(f"Labels    : {len(label_vocab)} ({', '.join(label_vocab[:8])}, ...)")
        log()

        log("Loading model (first run downloads weights from HuggingFace)...")
        model = load_model(args.model)
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
            expected_types = set(expected_pii.split(",")) if expected_has_pii else set()

            try:
                detected = detect_entity_types(
                    content,
                    model,
                    label_vocab,
                    args.threshold,
                    args.chunk_words,
                    args.chunk_overlap,
                )
            except Exception as exc:
                counts["ERROR"] += 1
                log(f"  [ERROR] {name}: {exc}")
                continue

            detected_has_pii = bool(detected)
            detected_str = ",".join(sorted(detected)) if detected else "none"

            type_coverage = None
            if expected_types:
                matched = expected_types & detected
                type_coverage = len(matched) / len(expected_types)
                type_coverage_sum += type_coverage
                type_coverage_n += 1

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

            cov_str = (
                f" type_coverage={type_coverage:.0%}"
                if type_coverage is not None
                else ""
            )
            if not passed:
                wrong_cases.append(
                    {
                        "name": name,
                        "kind": kind,
                        "expected_pii": expected_pii,
                        "detected_types": detected_str,
                        "type_coverage": type_coverage,
                        "note": case.get("note", ""),
                    }
                )
                log(
                    f"  [FAIL][{kind}] {name}: expected_pii={expected_pii} detected={detected_str}{cov_str}"
                )
            elif args.verbose:
                log(f"  [PASS][{kind}] {name}: detected={detected_str}{cov_str}")

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
        avg_type_coverage = (
            type_coverage_sum / type_coverage_n if type_coverage_n else 0.0
        )
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
        log(
            f"  Avg type coverage (bonus)      : {avg_type_coverage:.1%}  <- fraction of a PII case's own labeled types actually found"
        )
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
                "threshold": args.threshold,
                "chunk_words": args.chunk_words,
                "chunk_overlap": args.chunk_overlap,
                "label_vocabulary": label_vocab,
                "n_cases": len(all_cases),
                "elapsed_seconds": elapsed_seconds,
                "counts": counts,
                "recall": recall,
                "miss_rate": miss_rate,
                "precision": precision,
                "false_positive_rate": false_positive_rate,
                "fbeta": fbeta,
                "avg_type_coverage": avg_type_coverage,
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
