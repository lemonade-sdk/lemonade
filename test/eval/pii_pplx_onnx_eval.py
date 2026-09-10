"""
ONNX counterpart of pii_pplx_eval.py for perplexity-ai/pplx-pii-masking.

Benchmarks the ONNX export produced by pplx_pii_masking2onnx.py against the
same PII/benign snapshot, so the ONNX and safetensors rows in the comparison
table differ in exactly one variable: the numeric backend (onnxruntime
CPUExecutionProvider vs. PyTorch eager). Everything else - tokenization,
truncation, chunking, BIOES span decoding, the has_pii signal, the metric
framing, and the per-case log format - is deliberately identical to
pii_pplx_eval.py so the two runs' logs can be diffed case by case.

Two choices make that "one variable" claim real, and both matter:

  1. Span decoding reuses the CHECKPOINT'S OWN ViterbiDecoder, not an argmax.
     The ONNX graph deliberately stops at raw per-token logits (see
     pplx_pii_masking2onnx.py) - the constrained BIOES Viterbi is not baked
     in. Scoring ONNX by argmax while the safetensors run was scored by
     Viterbi would confound backend differences with decoder differences and
     make any delta uninterpretable. This script therefore imports the real
     ViterbiDecoder class out of the checkpoint's trust_remote_code module and
     feeds it the ONNX logits. It builds the decoder from the config's own
     viterbi_b_bias / viterbi_e_bias, exactly as PiiMaskingModel.__init__
     does, WITHOUT loading the 1.2 GB of weights - the decoder's state is the
     label list plus those two bias scalars, nothing learned.

  2. Tokenization mirrors PiiMaskingModel.predict() exactly: offsets on,
     truncation to config.max_seq_len, and the tokenizer's default special
     handling (this tokenizer adds no BOS/EOS). Chunking above the cap is the
     same token-offset routine as the safetensors run, so it stays inert on
     the Nemotron corpus (longest document ~1.7k tokens) there too.

Sensitivity head: read from the graph's second output ("sensitivity_logits")
and sigmoided, matching predict(). As in pii_pplx_eval.py it is tallied
separately and is NEVER the primary has_pii signal - on the safetensors 20k
run its recall was 0.092 vs. the span head's 0.992, so routing on it would
leak most PII.

Parity: pass --parity-against <safetensors run .log> to diff this run against
that one case by case at the end - decision agreement, exact label-set
agreement, and sensitivity delta. Requires the reference run used --verbose
(a non-verbose log records only failures). Sensitivity in those logs is
printed at 3 decimals, so sensitivity parity resolves to +/-0.001; label-set
parity is exact.

Requirements:
    pip install onnxruntime transformers torch

Usage:
    python test/eval/pii_pplx_onnx_eval.py [--corpus-dir DIR] [--onnx-path FILE] [--model PATH] [--limit N] [--verbose]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron
    --onnx-path      ~/.cache/lemonade-onnx-models/pplx-pii-masking-onnx/model.onnx
    --model          perplexity-ai/pplx-pii-masking  (tokenizer + config + the
                     trust_remote_code module holding ViterbiDecoder; no
                     weights are loaded)
    --limit          0  (all cases)
    --max-seq-len    0  (0 = take config.max_seq_len, i.e. 4096)
    --chunk-overlap  128
    --sensitivity-threshold 0.5  (secondary head only)
    --intra-op-threads 0  (0 = onnxruntime default)
    --progress-every 50
    --log-dir        <corpus-dir>/runs/
    --resume-from    none

Examples:
    # Smoke test, then check it agrees with the safetensors run case for case
    python test/eval/pii_pplx_onnx_eval.py --limit 20 --verbose

    # Full 20k run with the parity diff against the saved safetensors log
    python test/eval/pii_pplx_onnx_eval.py \
        --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose \
        --parity-against test/conformance/routing/1/l2_pii_nemotron_20k/runs/pplx_f1f90a53823f5df0a1344c1e137d9fffdaab54d6_20260908-173941.log
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


DEFAULT_ONNX_PATH = (
    Path.home()
    / ".cache"
    / "lemonade-onnx-models"
    / "pplx-pii-masking-onnx"
    / "model.onnx"
)


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
        help="Directory containing cases.jsonl",
    )
    p.add_argument(
        "--onnx-path",
        default=str(DEFAULT_ONNX_PATH),
        help="Path to model.onnx from pplx_pii_masking2onnx.py",
    )
    p.add_argument(
        "--model",
        default="perplexity-ai/pplx-pii-masking",
        help="HF repo id or local snapshot dir supplying the tokenizer, config, "
        "and the trust_remote_code module that defines ViterbiDecoder. No model "
        "weights are loaded from it.",
    )
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Only evaluate the first N cases (0 = all, the default).",
    )
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument(
        "--max-seq-len",
        type=int,
        default=0,
        help="Token cap per chunk. 0 (default) takes config.max_seq_len so this "
        "run can't silently disagree with the checkpoint's own predict().",
    )
    p.add_argument(
        "--chunk-overlap",
        type=int,
        default=128,
        help="Token overlap between consecutive chunks. Must match the "
        "safetensors run for the parity diff to be meaningful.",
    )
    p.add_argument(
        "--sensitivity-threshold",
        type=float,
        default=0.5,
        help="Threshold for the SECONDARY sensitivity head. Never the primary signal.",
    )
    p.add_argument(
        "--intra-op-threads",
        type=int,
        default=0,
        help="onnxruntime intra-op thread count (0 = its default). Set this to "
        "make a runtime comparison against the torch run fair, since torch used "
        "all cores.",
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
        help="Directory for the log + JSON summary. Defaults to <corpus-dir>/runs/.",
    )
    p.add_argument(
        "--no-log-file", action="store_true", help="Don't write a backup log file"
    )
    p.add_argument(
        "--resume-from",
        default=None,
        help="Path to a prior ONNX run's .log to resume after an interruption.",
    )
    p.add_argument(
        "--parity-against",
        default=None,
        help="Path to the safetensors run's .log to diff against, case by case, "
        "after the run. Requires that run used --verbose.",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Model / decoder
# ---------------------------------------------------------------------------


def load_onnx(onnx_path: Path, intra_op_threads: int):
    import onnxruntime as ort

    if not onnx_path.exists():
        print(
            f"ERROR: {onnx_path} not found - run pplx_pii_masking2onnx.py first, "
            "or pass --onnx-path.",
            file=sys.stderr,
        )
        sys.exit(1)

    options = ort.SessionOptions()
    if intra_op_threads > 0:
        options.intra_op_num_threads = intra_op_threads
    session = ort.InferenceSession(
        str(onnx_path), sess_options=options, providers=["CPUExecutionProvider"]
    )
    return session


def load_tokenizer_and_decoder(model_name: str):
    """Tokenizer, the checkpoint's real ViterbiDecoder, and config.max_seq_len.

    AutoConfig with trust_remote_code=True is enough to fetch and import the
    checkpoint's vendored modeling module, which is where ViterbiDecoder and
    BIOES_LABELS live - so the decoder comes from the checkpoint rather than a
    hand-copied label list, without paying to materialize any weights.
    """
    from transformers import AutoConfig, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(model_name)
    config = AutoConfig.from_pretrained(model_name, trust_remote_code=True)

    modeling_modules = [k for k in sys.modules if k.endswith("modeling_pii_masking")]
    if not modeling_modules:
        print(
            "ERROR: the checkpoint's modeling_pii_masking module was not imported "
            "by AutoConfig(trust_remote_code=True); cannot reach the real "
            "ViterbiDecoder.",
            file=sys.stderr,
        )
        sys.exit(1)
    modeling = sys.modules[modeling_modules[0]]

    decoder = modeling.ViterbiDecoder(
        modeling.BIOES_LABELS, config.viterbi_b_bias, config.viterbi_e_bias
    )
    decoder.eval()
    return tokenizer, decoder, int(config.max_seq_len)


def split_into_chunks(
    text: str, tokenizer, max_seq_len: int, overlap: int
) -> list[str]:
    """Token-offset chunking, character-identical to pii_pplx_eval.py's.

    Kept byte-for-byte equivalent to the safetensors script's routine on
    purpose: if the two runs chunked differently, a parity delta on a long
    document would be a harness artifact rather than a backend difference.
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
    text: str, tokenizer, decoder, session, max_seq_len: int, overlap: int
) -> tuple[set[str], float]:
    """(set of span labels, sensitivity) from the ONNX graph + real Viterbi.

    Mirrors PiiMaskingModel.predict(): tokenize with offsets and truncation,
    run the graph, decode the per-token logits with the checkpoint's Viterbi,
    sigmoid the sensitivity logit. Over chunks, labels union and sensitivity
    takes the max, same as the safetensors run.
    """
    import numpy as np
    import torch

    labels: set[str] = set()
    sensitivity = 0.0
    for chunk in split_into_chunks(text, tokenizer, max_seq_len, overlap):
        if not chunk.strip():
            continue
        enc = tokenizer(
            chunk,
            return_offsets_mapping=True,
            return_tensors="np",
            truncation=True,
            max_length=max_seq_len,
        )
        if enc["input_ids"].shape[1] == 0:
            continue
        logits, sensitivity_logits = session.run(
            ["logits", "sensitivity_logits"],
            {
                "input_ids": enc["input_ids"].astype(np.int64),
                "attention_mask": enc["attention_mask"].astype(np.int64),
            },
        )
        offsets = [tuple(o) for o in enc["offset_mapping"][0].tolist()]
        spans = decoder.decode(torch.from_numpy(logits[0]).float(), offsets, text=chunk)
        labels.update(s.label for s in spans)
        chunk_sensitivity = float(
            torch.from_numpy(np.asarray(sensitivity_logits).reshape(-1)[:1])
            .float()
            .sigmoid()
        )
        sensitivity = max(sensitivity, chunk_sensitivity)
    return labels, sensitivity


# ---------------------------------------------------------------------------
# Resume / parity
# ---------------------------------------------------------------------------

_RESUME_LINE_RE = re.compile(
    r"^  \[(?:(FAIL|PASS)\]\[(TP|TN|FP|FN)|ERROR)\] ([^\s:]+)(?:: (?:expected_pii=\S* )?detected=(\S+))?"
)

# Per-case line as written by both this script and pii_pplx_eval.py.
_CASE_LINE_RE = re.compile(
    r"^  \[(?:PASS|FAIL)\]\[(TP|TN|FP|FN)\] ([^\s:]+): "
    r"(?:expected_pii=\S+ )?detected=(\S+) sensitivity=([0-9.]+)"
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


def parse_case_results(log_path: Path) -> dict[str, dict]:
    """name -> {kind, detected, sensitivity} for every logged case."""
    results: dict[str, dict] = {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _CASE_LINE_RE.match(line)
        if not m:
            continue
        kind, name, detected, sensitivity = m.groups()
        results[name] = {
            "kind": kind,
            "detected": detected,
            "sensitivity": float(sensitivity),
        }
    return results


def parity_report(reference_log: Path, this_run: dict[str, dict], log) -> dict:
    """Diff this run against a reference run's log, case by case."""
    reference = parse_case_results(reference_log)

    log("=" * 60)
    log("PARITY vs. SAFETENSORS")
    log("=" * 60)
    log(f"  reference log : {reference_log}")
    log(f"  reference cases logged : {len(reference)}")
    log(f"  this run cases         : {len(this_run)}")

    shared = sorted(set(reference) & set(this_run))
    if not shared:
        log(
            "  ERROR: no case names in common - was the reference run made with "
            "--verbose? A non-verbose log records only failures."
        )
        return {"comparable_cases": 0}

    decision_agree = 0
    labels_agree = 0
    max_sens_delta = 0.0
    disagreements: list[dict] = []
    for name in shared:
        ref, cur = reference[name], this_run[name]
        ref_has = ref["detected"] != "none"
        cur_has = cur["detected"] != "none"
        if ref_has == cur_has:
            decision_agree += 1
        if ref["detected"] == cur["detected"]:
            labels_agree += 1
        else:
            disagreements.append(
                {
                    "name": name,
                    "safetensors": ref["detected"],
                    "onnx": cur["detected"],
                    "decision_flip": ref_has != cur_has,
                }
            )
        max_sens_delta = max(
            max_sens_delta, abs(ref["sensitivity"] - cur["sensitivity"])
        )

    n = len(shared)
    decision_flips = sum(1 for d in disagreements if d["decision_flip"])
    log(f"  comparable cases       : {n}")
    log(
        f"  has_pii decision agree : {decision_agree}/{n} "
        f"({decision_agree / n:.4%})  <- the routing-relevant number"
    )
    log(f"  routing decision flips : {decision_flips}")
    log(f"  exact label-set agree  : {labels_agree}/{n} ({labels_agree / n:.4%})")
    log(
        f"  max |sensitivity delta|: {max_sens_delta:.4f} "
        "(reference log rounds to 3dp, so ~0.001 is the noise floor)"
    )
    log()

    if disagreements:
        log(f"  LABEL-SET DISAGREEMENTS ({len(disagreements)})")
        for d in disagreements[:50]:
            flag = "  [DECISION FLIP]" if d["decision_flip"] else ""
            log(f"    {d['name']}{flag}")
            log(f"      safetensors: {d['safetensors']}")
            log(f"      onnx       : {d['onnx']}")
        if len(disagreements) > 50:
            log(f"    ... and {len(disagreements) - 50} more (see JSON summary)")
        log()

    return {
        "reference_log": str(reference_log),
        "comparable_cases": n,
        "decision_agreement": decision_agree / n,
        "decision_flips": decision_flips,
        "label_set_agreement": labels_agree / n,
        "max_sensitivity_delta": max_sens_delta,
        "disagreements": disagreements,
    }


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
    sens_counts = {"TP": 0, "TN": 0, "FP": 0, "FN": 0}
    wrong_cases: list[dict] = []
    this_run: dict[str, dict] = {}
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
        this_run.update(parse_case_results(resume_log_path))
        # Rebuild the secondary head's tally too. parse_resume_log restores only
        # the primary counts, so without this a resumed run reports a
        # sensitivity-head recall scoped to the post-resume cases while the
        # primary metrics cover the whole corpus - two different denominators in
        # one summary. Resolution here is the log's 3dp rounding, so a resumed
        # tally can differ by a case or two right at the threshold.
        for info in this_run.values():
            resumed_flag = info["sensitivity"] >= args.sensitivity_threshold
            if info["kind"] in ("TP", "FN"):
                sens_counts["TP" if resumed_flag else "FN"] += 1
            else:
                sens_counts["FP" if resumed_flag else "TN"] += 1
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
            run_id = f"pplx_onnx_{time.strftime('%Y%m%d-%H%M%S')}"
            log_path = log_dir / f"{run_id}.log"
            json_path = log_dir / f"{run_id}.json"
            log_file_handle = log_path.open("w", encoding="utf-8", buffering=1)

    def log(msg: str = "") -> None:
        print(msg, flush=True)
        if log_file_handle:
            log_file_handle.write(msg + "\n")
            log_file_handle.flush()

    try:
        onnx_path = Path(args.onnx_path)
        log(f"\n--- resuming{resume_note} ---" if args.resume_from else "")
        log(f"Corpus      : {corpus_dir}")
        log(f"ONNX graph  : {onnx_path}")
        log(f"Tokenizer   : {args.model}")
        log(f"Cases       : {len(all_cases)}{resume_note}")
        log()

        log("Loading tokenizer + the checkpoint's ViterbiDecoder (no weights)...")
        tokenizer, decoder, config_max_seq_len = load_tokenizer_and_decoder(args.model)
        max_seq_len = args.max_seq_len if args.max_seq_len > 0 else config_max_seq_len
        log(f"  BIOES labels : {decoder.num_labels}")
        log(f"  max_seq_len  : {max_seq_len} (config says {config_max_seq_len})")

        log("Loading ONNX graph with CPUExecutionProvider...")
        session = load_onnx(onnx_path, args.intra_op_threads)
        log(f"  providers    : {session.get_providers()}")
        graph_outputs = [o.name for o in session.get_outputs()]
        log(f"  outputs      : {graph_outputs}")
        for required in ("logits", "sensitivity_logits"):
            if required not in graph_outputs:
                log(
                    f"ERROR: graph is missing the '{required}' output; this is not "
                    "the export pplx_pii_masking2onnx.py produces."
                )
                sys.exit(1)
        log("Ready.\n")

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
                    content,
                    tokenizer,
                    decoder,
                    session,
                    max_seq_len,
                    args.chunk_overlap,
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
            this_run[name] = {
                "kind": kind,
                "detected": detected_str,
                "sensitivity": round(sensitivity, 3),
            }

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
        log("CONFUSION MATRIX (primary: span head via ONNX + real Viterbi)")
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

        parity = None
        if args.parity_against:
            parity = parity_report(Path(args.parity_against), this_run, log)

        log("=" * 60)
        overall = counts["FN"] == 0 and counts["ERROR"] == 0
        result = "PASS - zero misses" if overall else "FAIL - see above"
        log(f"RESULT: {result}")
        log("=" * 60)

        if json_path:
            summary = {
                "run_id": run_id,
                "corpus_dir": str(corpus_dir),
                "onnx_path": str(onnx_path),
                "tokenizer_model": args.model,
                "backend": "onnxruntime-CPUExecutionProvider",
                "intra_op_threads": args.intra_op_threads,
                "max_seq_len": max_seq_len,
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
                "parity": parity,
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
