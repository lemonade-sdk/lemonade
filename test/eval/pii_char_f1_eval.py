"""
Character-level PII detection scoring for the ONNX detector models.

Document-level scoring answers "did the model fire anywhere in this document",
which is the right question for routing and a useless one for ranking models
that all sit between 0.00% and 0.80% leak. It also cannot measure precision at
all on this corpus, because the corpus has one benign document (see
pii_benchmarking.md 2.1). This script scores the same models the same runs
did, but over CHARACTERS rather than documents.

The move that makes it work: stop asking what the model called a span and ask
which characters it covered. A prediction and a gold entity at the same offsets
agree whether the model said `account_number` and the dataset said
`medical_record_number` or not, so no taxonomy mapping enters the arithmetic
and pii_taxonomy.py's editorial judgment stops being load-bearing.

Two things this buys that nothing else in the series does:

  - PRECISION WITHOUT A BENIGN ARM. An over-tagging model is penalized on
    positive documents, so the "a model that flagged everything would score
    perfectly" hole in every document-level table closes. It does not close
    completely: the ceiling is the document's PII density, so flagging every
    character scores precision ~0.13 on this corpus rather than 0.

  - WRONG LABEL SEPARATED FROM WRONG LOCATION. mmBERT emits nine label names
    on case 15485 that the document does not contain, while 35 of its 36
    flagged characters are genuinely PII. Label-set scoring calls that nine
    errors; character scoring calls it a correct detection with a bad name.

Character F1, not span-exact F1, and that is deliberate. Models fragment: mmBERT
emits one ID number as six tokens, pplx emits one email as four. Span-exact
matching scores near-zero on detections that are entirely correct.

WHITESPACE IS EXCLUDED FROM BOTH SIDES of the primary metric. Subword tokenizers
fold the preceding space into a token, so a model is otherwise rewarded for
"finding" the space before a name it detected, and models with different
tokenizers get different amounts of that free credit. The raw
whitespace-inclusive figures are reported alongside; the gap between them is
that artifact's size.

THREE DECISION RULES, one forward pass, because pii_benchmarking.md 6.4 showed
a 2x leak-rate difference between two of them that had nothing to do with the
backend:

  argmax     per-token argmax != O. What pii_ner_eval.py scored.
  min_score  per-token max non-O softmax >= --min-score. What the shipped
             router policies score (l2_pii_onnx_*/policy.json, all at 0.5).
             Softmax over the label set sums to 1, so at >= 0.5 this is a
             strict subset of argmax, not a tunable variant of it.
  viterbi    the checkpoint's own constrained BIOES decoder. pplx only, and
             the primary rule there: its ONNX graph deliberately stops at raw
             logits, so scoring it by argmax would measure decoder-vs-decoder.

Special tokens are excluded from every rule via the tokenizer's own
special_tokens_mask. mmBERT's <bos> unconditionally predicts a label; including
it makes every document a detection.

PREDICTED SPANS ARE WRITTEN TO DISK (--spans-out, on by default). The reason
the whole series had to re-run inference to get here is that no run ever
recorded where a model fired. Every later re-scoring - per-label character F1,
a different threshold, span-exact as a cross-check - reads that file instead.

Requirements:
    pip install onnxruntime transformers torch numpy

Usage:
    python test/eval/pii_char_f1_eval.py --model {pplx,mmbert,privacy-filter} \\
        [--corpus-dir DIR] [--limit N] [--verbose]

Defaults:
    --corpus-dir     test/conformance/routing/1/l2_pii_nemotron_20k
    --min-score      0.5   (matches every committed l2_pii_onnx_*/policy.json)
    --limit          0     (all cases)
    --intra-op-threads 0   (0 = onnxruntime default)
    --log-dir        <corpus-dir>/runs/

Examples:
    # Phase 1: the cheap slice that decides whether the full run is worth it
    python test/eval/pii_char_f1_eval.py --model pplx --limit 500 --verbose

    # Full corpus, all three detectors
    for m in pplx mmbert privacy-filter; do
        python test/eval/pii_char_f1_eval.py --model $m --verbose
    done
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
# Model registry
# ---------------------------------------------------------------------------
#
# repo/subfolder identify the ONNX export; the three entries are exactly the
# models the committed router policies classify with, so a character number
# here refers to the same artifact a policy row does.

MODELS = {
    "pplx": {
        "repo": "satyadevineni/pplx-pii-masking-onnx",
        "subfolder": "",
        "tokenizer": "satyadevineni/pplx-pii-masking-onnx",
        "labels_from": "pplx-module",
        "decoder": "viterbi",
        "max_length": 4096,
        "policy": "l2_pii_onnx_pplx_masking",
        "note": "perplexity-ai/pplx-pii-masking, 9 types, BIOES, dual-head",
    },
    "mmbert": {
        "repo": "llm-semantic-router/mmbert32k-pii-detector-merged",
        "subfolder": "onnx",
        "tokenizer": "llm-semantic-router/mmbert32k-pii-detector-merged",
        "labels_from": "config",
        "decoder": "bio",
        "max_length": 8192,
        "policy": "l2_pii_onnx_classifier",
        "note": "mmBERT32K-PII, Presidio's 17 types, BIO",
    },
    "privacy-filter": {
        "repo": "lemonade-sdk/openmed-privacy-filter-multilingual-v2-onnx",
        "subfolder": "",
        "tokenizer": "lemonade-sdk/openmed-privacy-filter-multilingual-v2-onnx",
        "labels_from": "config",
        "decoder": "bioes",
        "max_length": 8192,
        "policy": "l2_pii_onnx_privacy_filter",
        "note": "OpenMed privacy-filter-ml-v2, 54 types, BIOES",
    },
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--model", required=True, choices=sorted(MODELS))
    p.add_argument(
        "--corpus-dir",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_nemotron_20k"
        ),
        help="Directory containing cases.jsonl with gold pii_spans",
    )
    p.add_argument("--onnx-path", default="", help="Override the resolved model.onnx")
    p.add_argument(
        "--min-score",
        type=float,
        default=0.5,
        help="Threshold for the router's decision rule",
    )
    p.add_argument("--limit", type=int, default=0, help="First N cases (0 = all)")
    p.add_argument("--verbose", action="store_true", help="Print per-case results")
    p.add_argument("--max-length", type=int, default=0, help="0 = the model's default")
    p.add_argument("--intra-op-threads", type=int, default=0)
    p.add_argument("--progress-every", type=int, default=200)
    p.add_argument("--log-dir", default="")
    p.add_argument("--no-log-file", action="store_true")
    p.add_argument(
        "--resume-from",
        default="",
        help="A prior run's .spans.jsonl to continue. Cases already in it are "
        "skipped and their metrics recomputed from the recorded spans, not "
        "re-inferred, so a resumed summary covers the whole corpus on one "
        "denominator. Appends to that same file.",
    )
    p.add_argument(
        "--spans-out",
        default="",
        help="Where to write per-case predicted spans (default: beside the log). "
        "Pass 'none' to skip - but then any re-scoring needs a new inference pass.",
    )
    return p.parse_args()


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------


def resolve_model(spec: dict, onnx_override: str):
    """(onnxruntime session inputs, tokenizer, labels, viterbi decoder or None)."""
    import json as _json

    from huggingface_hub import snapshot_download
    from transformers import AutoTokenizer

    root = Path(
        snapshot_download(
            spec["repo"],
            allow_patterns=["*.json", "*.onnx", "*.onnx.data", "*.txt", "*.model"],
        )
    )
    model_dir = root / spec["subfolder"] if spec["subfolder"] else root
    onnx_path = Path(onnx_override) if onnx_override else model_dir / "model.onnx"
    if not onnx_path.exists():
        print(f"ERROR: {onnx_path} not found", file=sys.stderr)
        sys.exit(1)

    tokenizer = AutoTokenizer.from_pretrained(str(model_dir))

    decoder = None
    if spec["labels_from"] == "pplx-module":
        from transformers import AutoConfig

        # The ONNX graph stops at raw logits on purpose, so the constrained
        # BIOES Viterbi has to come from the checkpoint itself. Its entire
        # state is the label list plus two bias scalars, so no weights load.
        config = AutoConfig.from_pretrained(
            "perplexity-ai/pplx-pii-masking", trust_remote_code=True
        )
        modules = [k for k in sys.modules if k.endswith("modeling_pii_masking")]
        if not modules:
            print(
                "ERROR: the checkpoint's modeling_pii_masking module was not "
                "imported; cannot reach the real ViterbiDecoder.",
                file=sys.stderr,
            )
            sys.exit(1)
        modeling = sys.modules[modules[0]]
        labels = list(modeling.BIOES_LABELS)
        decoder = modeling.ViterbiDecoder(
            labels, config.viterbi_b_bias, config.viterbi_e_bias
        )
        decoder.eval()
    else:
        raw = _json.loads((model_dir / "config.json").read_text(encoding="utf-8"))
        id2label = raw["id2label"]
        labels = [id2label[str(i)] for i in range(len(id2label))]

    return onnx_path, tokenizer, labels, decoder


def make_session(onnx_path: Path, intra_op_threads: int):
    import onnxruntime as ort

    options = ort.SessionOptions()
    if intra_op_threads > 0:
        options.intra_op_num_threads = intra_op_threads
    return ort.InferenceSession(
        str(onnx_path), options, providers=["CPUExecutionProvider"]
    )


# ---------------------------------------------------------------------------
# Character sets
# ---------------------------------------------------------------------------


def char_set(spans, text: str, drop_whitespace: bool) -> set[int]:
    """The character indices a list of (start, end) spans covers."""
    out: set[int] = set()
    for start, end in spans:
        for i in range(start, end):
            if drop_whitespace and (i >= len(text) or text[i].isspace()):
                continue
            out.add(i)
    return out


def prf(tp: int, fp: int, fn: int) -> tuple[float, float, float]:
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = (
        2 * precision * recall / (precision + recall) if precision + recall else 0.0
    )
    return precision, recall, f1


def merge(spans: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Overlapping spans merged, so a span list is a clean character region."""
    if not spans:
        return []
    ordered = sorted(spans)
    out = [list(ordered[0])]
    for start, end in ordered[1:]:
        if start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return [(a, b) for a, b in out]


def trim(spans, text: str) -> list[tuple[int, int]]:
    """Whitespace shaved off span edges, the way pplx's own decoder does it."""
    out = []
    for start, end in spans:
        while start < end and text[start].isspace():
            start += 1
        while end > start and text[end - 1].isspace():
            end -= 1
        if start < end:
            out.append((start, end))
    return out


# ---------------------------------------------------------------------------
# Prediction
# ---------------------------------------------------------------------------


def predict(text, tokenizer, session, labels, decoder, max_length, min_score):
    """Predicted character spans per decision rule, plus the labels fired.

    Returns {rule: [(start, end), ...]} for "argmax" and "min_score", plus
    "viterbi" when the model ships a decoder, and the set of label names -
    which keeps the existing document-level `detected=` field reproducible
    from the same pass.
    """
    import numpy as np

    encoded = tokenizer(
        text,
        return_offsets_mapping=True,
        return_special_tokens_mask=True,
        return_tensors="np",
        truncation=True,
        max_length=max_length,
    )
    special = encoded.pop("special_tokens_mask")[0]
    offsets = encoded.pop("offset_mapping")[0]
    if encoded["input_ids"].shape[1] == 0:
        return {"argmax": [], "min_score": [], "viterbi": []}, set(), 0.0

    outputs = session.run(
        None,
        {
            "input_ids": encoded["input_ids"].astype(np.int64),
            "attention_mask": encoded["attention_mask"].astype(np.int64),
        },
    )
    logits = outputs[0][0]
    sensitivity = 0.0
    if len(outputs) > 1:
        raw = float(np.asarray(outputs[1]).reshape(-1)[0])
        sensitivity = float(1.0 / (1.0 + np.exp(-raw)))

    shifted = logits - logits.max(axis=-1, keepdims=True)
    exp = np.exp(shifted)
    probs = exp / exp.sum(axis=-1, keepdims=True)
    best = logits.argmax(axis=-1)

    o_index = labels.index("O")
    non_o = probs.copy()
    non_o[:, o_index] = -1.0
    non_o_best = non_o.max(axis=-1)

    argmax_spans: list[tuple[int, int]] = []
    threshold_spans: list[tuple[int, int]] = []
    fired: set[str] = set()
    for token, is_special in enumerate(special.tolist()):
        if is_special:
            continue
        start, end = int(offsets[token][0]), int(offsets[token][1])
        if start >= end:
            continue
        if int(best[token]) != o_index:
            argmax_spans.append((start, end))
            fired.add(labels[int(best[token])].split("-", 1)[-1])
        if float(non_o_best[token]) >= min_score:
            threshold_spans.append((start, end))

    result = {
        "argmax": merge(trim(argmax_spans, text)),
        "min_score": merge(trim(threshold_spans, text)),
        "viterbi": [],
    }

    if decoder is not None:
        import torch

        decoded = decoder.decode(
            torch.from_numpy(logits).float(),
            [tuple(int(v) for v in pair) for pair in offsets.tolist()],
            text=text,
        )
        result["viterbi"] = merge(trim([(s.start, s.end) for s in decoded], text))
        fired = {s.label for s in decoded}

    return result, fired, sensitivity


# ---------------------------------------------------------------------------
# Evaluate
# ---------------------------------------------------------------------------


def evaluate(args: argparse.Namespace) -> None:
    spec = MODELS[args.model]
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
    if not any("pii_spans" in c for c in cases):
        print(
            "ERROR: no case carries `pii_spans`. Run annotate_gold_spans.py on "
            "this corpus first - character scoring needs gold offsets, and no "
            "amount of re-parsing recovers them from the label list alone.",
            file=sys.stderr,
        )
        sys.exit(1)
    if args.limit > 0:
        cases = cases[: args.limit]

    print(f"Model : {args.model}  ({spec['note']})")
    onnx_path, tokenizer, labels, decoder = resolve_model(spec, args.onnx_path)
    session = make_session(onnx_path, args.intra_op_threads)
    max_length = args.max_length or spec["max_length"]
    print(f"ONNX  : {onnx_path}")
    print(f"Labels: {len(labels)}   max_length: {max_length}")
    print(f"Rules : argmax, min_score@{args.min_score}"
          + (", viterbi" if decoder is not None else ""))
    print(f"Cases : {len(cases):,}")
    print()

    rules = ["argmax", "min_score"] + (["viterbi"] if decoder is not None else [])
    primary = "viterbi" if decoder is not None else "argmax"

    # A resumed run recomputes the earlier cases' metrics from their recorded
    # spans rather than re-inferring them. pii_benchmarking.md 8.13: a resume
    # that reconstructs only some tallies reports two denominators in one
    # summary. Replaying the spans keeps every metric on the same one.
    replayed: dict[str, dict] = {}
    if args.resume_from:
        resume_path = Path(args.resume_from)
        if not resume_path.exists():
            print(f"ERROR: {resume_path} not found", file=sys.stderr)
            sys.exit(1)
        for line in resume_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                break  # an interrupted run's last line can be half-written
            replayed[record["name"]] = record
        print(f"Resuming: {len(replayed):,} cases replayed from {resume_path}")

    totals = {
        rule: {"tp": 0, "fp": 0, "fn": 0, "tp_raw": 0, "fp_raw": 0, "fn_raw": 0}
        for rule in rules
    }
    macro = {rule: [] for rule in rules}
    doc = {rule: {"tp": 0, "fn": 0, "fp": 0, "tn": 0} for rule in rules}
    truncated = 0
    errors = 0
    reused = 0

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    log_lines: list[str] = []

    def log(message: str = "") -> None:
        print(message)
        log_lines.append(message)

    spans_file = None
    if args.spans_out != "none":
        spans_path = (
            Path(args.spans_out)
            if args.spans_out
            else Path(args.resume_from)
            if args.resume_from
            else (Path(args.log_dir) if args.log_dir else corpus_dir / "runs")
            / f"charf1_{args.model}_{timestamp}.spans.jsonl"
        )
        spans_path.parent.mkdir(parents=True, exist_ok=True)
        appending = args.resume_from and spans_path == Path(args.resume_from)
        spans_file = spans_path.open("a" if appending else "w", encoding="utf-8")
        print(f"Predicted spans -> {spans_path}")
        print()

    started = time.time()
    for index, case in enumerate(cases, 1):
        name = case["name"]
        text = case["request"]["messages"][0]["content"]
        gold_spans = [(s["start"], s["end"]) for s in case.get("pii_spans", [])]
        gold_clean = char_set(merge(gold_spans), text, True)
        gold_raw = char_set(merge(gold_spans), text, False)

        prior = replayed.get(name)
        if prior is not None:
            predicted = {
                rule: [tuple(span) for span in prior["spans"].get(rule, [])]
                for rule in rules
            }
            fired = set(prior.get("labels", []))
            sensitivity = float(prior.get("sensitivity", 0.0))
            reused += 1
        else:
            try:
                predicted, fired, sensitivity = predict(
                    text, tokenizer, session, labels, decoder, max_length,
                    args.min_score,
                )
            except Exception as exc:  # noqa: BLE001 - one bad case must not kill a 20k run
                errors += 1
                log(f"  [ERROR] {name}: {type(exc).__name__}: {exc}")
                continue
            if len(tokenizer(text)["input_ids"]) > max_length:
                truncated += 1

        per_case = {}
        for rule in rules:
            spans = predicted[rule]
            clean = char_set(spans, text, True)
            raw = char_set(spans, text, False)
            tp, fp, fn = (
                len(clean & gold_clean),
                len(clean - gold_clean),
                len(gold_clean - clean),
            )
            totals[rule]["tp"] += tp
            totals[rule]["fp"] += fp
            totals[rule]["fn"] += fn
            totals[rule]["tp_raw"] += len(raw & gold_raw)
            totals[rule]["fp_raw"] += len(raw - gold_raw)
            totals[rule]["fn_raw"] += len(gold_raw - raw)
            if gold_clean or clean:
                macro[rule].append(prf(tp, fp, fn)[2])
            key = "tp" if gold_spans and spans else (
                "fn" if gold_spans else ("fp" if spans else "tn")
            )
            doc[rule][key] += 1
            per_case[rule] = prf(tp, fp, fn)

        if spans_file is not None and prior is None:
            spans_file.write(
                json.dumps(
                    {
                        "name": name,
                        "n_gold_chars": len(gold_clean),
                        "sensitivity": round(sensitivity, 6),
                        "labels": sorted(fired),
                        "spans": {rule: predicted[rule] for rule in rules},
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )

        if args.verbose:
            precision, recall, f1 = per_case[primary]
            verdict = "PASS" if (not gold_spans or predicted[primary]) else "FAIL"
            log(
                f"  [{verdict}] {name}: charP={precision:.3f} charR={recall:.3f} "
                f"charF1={f1:.3f} gold_chars={len(gold_clean)} "
                f"detected={','.join(sorted(fired)) or 'none'}"
            )
        if args.progress_every and index % args.progress_every == 0:
            rate = index / max(1e-9, time.time() - started)
            remaining = (len(cases) - index) / max(1e-9, rate)
            print(
                f"  ... {index:,}/{len(cases):,}  {rate:.1f} cases/s  "
                f"eta {remaining / 60:.1f} min",
                file=sys.stderr,
            )

    elapsed = time.time() - started
    if spans_file is not None:
        spans_file.close()

    gold_chars = totals[primary]["tp"] + totals[primary]["fn"]
    # Non-whitespace, to match the metric: whitespace is excluded from both
    # gold and predictions, so the density a flag-everything model would score
    # has to be over the same denominator or the floor comes out too low.
    prompt_chars = sum(
        sum(1 for ch in c["request"]["messages"][0]["content"] if not ch.isspace())
        for c in cases
    )

    log("=" * 72)
    log(f"CHARACTER-LEVEL RESULTS - {args.model}")
    log("=" * 72)
    log(f"  corpus            : {corpus_dir}")
    log(f"  cases             : {len(cases):,}   errors: {errors}   "
        f"truncated: {truncated}   replayed from resume: {reused:,}")
    log(f"  gold PII chars    : {gold_chars:,} of {prompt_chars:,} non-whitespace "
        f"prompt chars ({gold_chars / max(1, prompt_chars):.4f} density)")
    log(f"  runtime           : {elapsed / 60:.1f} min "
        f"({len(cases) / max(1e-9, elapsed):.1f} cases/s)")
    log()
    log("  Whitespace excluded from both sides (the number to quote):")
    log(f"  {'rule':<12} {'char P':>8} {'char R':>8} {'char F1':>8} "
        f"{'macro F1':>9} {'doc recall':>11}")
    summary_rules = {}
    for rule in rules:
        t = totals[rule]
        precision, recall, f1 = prf(t["tp"], t["fp"], t["fn"])
        macro_f1 = sum(macro[rule]) / len(macro[rule]) if macro[rule] else 0.0
        d = doc[rule]
        doc_recall = d["tp"] / max(1, d["tp"] + d["fn"])
        marker = " *" if rule == primary else ""
        log(f"  {rule:<12} {precision:>8.4f} {recall:>8.4f} {f1:>8.4f} "
            f"{macro_f1:>9.4f} {doc_recall:>11.4%}{marker}")
        precision_raw, recall_raw, f1_raw = prf(t["tp_raw"], t["fp_raw"], t["fn_raw"])
        summary_rules[rule] = {
            "char_precision": precision,
            "char_recall": recall,
            "char_f1": f1,
            "macro_char_f1": macro_f1,
            "char_precision_with_whitespace": precision_raw,
            "char_recall_with_whitespace": recall_raw,
            "char_f1_with_whitespace": f1_raw,
            "tp_chars": t["tp"],
            "fp_chars": t["fp"],
            "fn_chars": t["fn"],
            "doc_tp": d["tp"],
            "doc_fn": d["fn"],
            "doc_fp": d["fp"],
            "doc_tn": d["tn"],
            "doc_recall": doc_recall,
            "doc_leak_rate": d["fn"] / max(1, d["tp"] + d["fn"]),
        }
    log(f"  * primary rule for this model")
    log()
    log("  Whitespace included (the artifact's size, not the number to quote):")
    for rule in rules:
        s = summary_rules[rule]
        log(f"  {rule:<12} {s['char_precision_with_whitespace']:>8.4f} "
            f"{s['char_recall_with_whitespace']:>8.4f} "
            f"{s['char_f1_with_whitespace']:>8.4f}")
    log()
    density = gold_chars / max(1, prompt_chars)
    log(f"  A model flagging every character would score "
        f"P={density:.4f} R=1.0000 F1={2 * density / (1 + density):.4f}. "
        f"That is the floor a character F1 has to beat, and the reason this "
        f"metric measures precision at all without a benign arm.")
    log()

    summary = {
        "model": args.model,
        "onnx_path": str(onnx_path),
        "corpus": str(corpus_dir),
        "policy": spec["policy"],
        "cases": len(cases),
        "errors": errors,
        "truncated": truncated,
        "replayed_from_resume": reused,
        "min_score": args.min_score,
        "max_length": max_length,
        "primary_rule": primary,
        "gold_chars": gold_chars,
        "prompt_chars": prompt_chars,
        "pii_density": density,
        "elapsed_sec": elapsed,
        "rules": summary_rules,
    }

    if not args.no_log_file:
        log_dir = Path(args.log_dir) if args.log_dir else corpus_dir / "runs"
        log_dir.mkdir(parents=True, exist_ok=True)
        base = log_dir / f"charf1_{args.model}_{timestamp}"
        base.with_suffix(".log").write_text("\n".join(log_lines), encoding="utf-8")
        base.with_suffix(".json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8"
        )
        print(f"Wrote {base.with_suffix('.log')}")
        print(f"Wrote {base.with_suffix('.json')}")


if __name__ == "__main__":
    evaluate(parse_args())
