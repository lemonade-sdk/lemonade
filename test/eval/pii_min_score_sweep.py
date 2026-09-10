"""
Re-score mmBERT32K-PII under the router's decision rule and sweep min_score.

Answers a question the existing runs cannot: how much of the gap between the
safetensors row (0.12%, 25/20000) and the ONNX/Lemonade row (0.24%, 49/20000)
in pii_benchmarking.md's table is the backend, and how much is the decision
rule. They are not the same rule:

  pii_ner_eval.py (safetensors)  argmax per token; fire if any non-special
                                 token's argmax label is not "O"
  the router (ort-server)        softmax per token -> max over tokens per label
                                 -> fire if any non-"O" label >= min_score

Because a per-token softmax over 35 labels sums to 1, a label scoring above 0.5
at a token is necessarily that token's argmax. So min_score >= 0.5 is *strictly
stricter* than argmax: everything the router catches, argmax catches too. The
converse fails whenever the model's best non-"O" evidence is a plurality rather
than a majority (B-PERSON 0.42 vs O 0.31), and those cases leak.

This script emulates ort-server's rule directly on the ONNX export - one
forward pass per case, no server, no router - and records the top non-"O"
aggregated score per case. From that single number per case, recall at any
threshold is arithmetic, so the whole sweep costs one pass.

Special tokens are excluded from the max. That is not a guess: including them
puts all 49 known router leaks at >= 0.5 (this model's <bos> always fires a
label, the same artifact pii_ner_eval.py documents), while excluding them
reproduces all 49 exactly. See --self-check.

Output is JSONL (one record per case, streamed) so an interrupted run keeps
what it computed; re-running with the same --out resumes.

Requirements:
    pip install onnxruntime transformers numpy

Usage:
    python test/eval/pii_min_score_sweep.py [--corpus-dir DIR] [--model-dir DIR] [--out FILE] [--ids-file FILE] [--limit N] [--max-length N] [--self-check LOG]

Examples:
    # Reproduce the 49 router leaks from a policy run's log, then exit
    python test/eval/pii_min_score_sweep.py --self-check test/conformance/routing/1/l2_pii_nemotron_20k/runs/policy_smoke_20260819-100935.log

    # Full corpus sweep
    python test/eval/pii_min_score_sweep.py
"""

import argparse
import json
import re
import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
from transformers import AutoTokenizer

DEFAULT_CORPUS = "test/conformance/routing/1/l2_pii_nemotron_20k"
DEFAULT_MODEL = Path.home() / ".cache" / "lemonade-onnx-models" / "mmbert32k-pii-onnx"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--corpus-dir", default=DEFAULT_CORPUS)
    p.add_argument("--model-dir", default=str(DEFAULT_MODEL))
    p.add_argument(
        "--out",
        default=None,
        help="JSONL output (default: <corpus-dir>/runs/min_score_sweep.jsonl)",
    )
    p.add_argument(
        "--ids-file", default=None, help="Score only the case ids listed in this file"
    )
    p.add_argument("--limit", type=int, default=0)
    p.add_argument(
        "--max-length",
        type=int,
        default=8192,
        help="Matches pii_ner_eval.py's default so the two runs truncate identically",
    )
    p.add_argument(
        "--self-check",
        default=None,
        help="A pii_routing_eval.py log; score its [FAIL][TP] leaks, verify they "
        "all land below 0.5, then exit",
    )
    p.add_argument("--progress-every", type=int, default=250)
    return p.parse_args()


def softmax(x: np.ndarray) -> np.ndarray:
    x = x - x.max(axis=-1, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis=-1, keepdims=True)


class Scorer:
    """One forward pass -> the router's aggregated per-label scores."""

    def __init__(self, model_dir: Path, max_length: int):
        manifest = json.loads((model_dir / "manifest.json").read_text(encoding="utf-8"))
        self.id2label = {int(k): v for k, v in manifest["id2label"].items()}
        self.non_o = np.array([i for i, l in self.id2label.items() if l != "O"])
        self.max_length = max_length
        self.tok = AutoTokenizer.from_pretrained(str(model_dir))
        self.sess = ort.InferenceSession(
            str(model_dir / "model.onnx"), providers=["CPUExecutionProvider"]
        )
        self.inputs = {i.name for i in self.sess.get_inputs()}

    def score(self, text: str) -> dict:
        enc = self.tok(
            text,
            return_tensors="np",
            truncation=True,
            max_length=self.max_length,
            return_special_tokens_mask=True,
        )
        special = enc.pop("special_tokens_mask")[0].astype(bool)
        feed = {k: v.astype(np.int64) for k, v in enc.items() if k in self.inputs}
        probs = softmax(self.sess.run(None, feed)[0][0].astype(np.float32))

        real = probs[~special]
        if real.size == 0:
            return {
                "top": 0.0,
                "label": "-",
                "argmax": False,
                "n_tokens": int(len(probs)),
                "truncated": False,
            }
        per_label = real[:, self.non_o].max(axis=0)
        j = int(per_label.argmax())
        return {
            "top": float(per_label[j]),
            "label": self.id2label[int(self.non_o[j])],
            "argmax": bool((real.argmax(axis=-1) != 0).any()),
            "n_tokens": int(len(probs)),
            "truncated": bool(len(probs) >= self.max_length),
        }


def load_cases(corpus_dir: Path) -> dict:
    cases = {}
    for line in (corpus_dir / "cases.jsonl").read_text(encoding="utf-8").splitlines():
        if line.strip():
            d = json.loads(line)
            cases[d["name"]] = d
    return cases


def run_self_check(args, cases, model_dir) -> None:
    text = Path(args.self_check).read_text(encoding="utf-8", errors="replace")
    ids = sorted(set(re.findall(r"^\s*\[FAIL\]\[TP\]\s+([A-Za-z0-9_-]+)", text, re.M)))
    print(f"Self-check: {len(ids)} leaked cases from {args.self_check}")
    if not ids:
        print("  no [FAIL][TP] lines found - nothing to check against")
        sys.exit(1)
    scorer = Scorer(model_dir, args.max_length)
    below = 0
    argmax_hits = 0
    for cid in ids:
        r = scorer.score(cases[cid]["request"]["messages"][0]["content"])
        below += r["top"] < 0.5
        argmax_hits += r["argmax"]
    print(f"  below min_score 0.5      : {below}/{len(ids)}")
    print(f"  argmax would have caught : {argmax_hits}/{len(ids)}")
    ok = below == len(ids)
    print(f"  emulation matches router : {ok}")
    sys.exit(0 if ok else 1)


def main() -> None:
    args = parse_args()
    corpus_dir = Path(args.corpus_dir)
    model_dir = Path(args.model_dir)
    cases = load_cases(corpus_dir)

    if args.self_check:
        run_self_check(args, cases, model_dir)

    out_path = (
        Path(args.out) if args.out else corpus_dir / "runs" / "min_score_sweep.jsonl"
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)

    done = set()
    if out_path.exists():
        for line in out_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                done.add(json.loads(line)["id"])
        print(f"Resuming: {len(done)} cases already scored in {out_path}")

    targets = list(cases)
    if args.ids_file:
        wanted = set(Path(args.ids_file).read_text().split())
        targets = [c for c in targets if c in wanted]
    if args.limit:
        targets = targets[: args.limit]
    todo = [c for c in targets if c not in done]

    print(f"Corpus    : {corpus_dir}")
    print(f"Model dir : {model_dir}")
    print(f"To score  : {len(todo)} of {len(targets)}")
    scorer = Scorer(model_dir, args.max_length)
    print("Model loaded.", flush=True)

    t0 = time.time()
    with out_path.open("a", encoding="utf-8", buffering=1) as fh:
        for n, cid in enumerate(todo, 1):
            case = cases[cid]
            r = scorer.score(case["request"]["messages"][0]["content"])
            r["id"] = cid
            r["pii_category"] = case.get("pii_category", "")
            fh.write(json.dumps(r) + "\n")
            if args.progress_every and n % args.progress_every == 0:
                elapsed = time.time() - t0
                rate = n / elapsed
                print(
                    f"  {n}/{len(todo)} ({elapsed:.0f}s, {rate:.2f} cases/s, "
                    f"eta {(len(todo) - n) / rate / 60:.0f}min)",
                    flush=True,
                )
    print(f"Done in {(time.time() - t0) / 60:.1f}min -> {out_path}")


if __name__ == "__main__":
    main()
