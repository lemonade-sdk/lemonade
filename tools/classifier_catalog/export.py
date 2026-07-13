"""Export a HuggingFace text/token classifier to ONNX for the Lemonade
`onnxruntime` backend (ort-server), and validate it against the original model.

This is the exact script used to produce the artifacts in the lemonade-sdk ONNX
classifier repos. Reproduce with:

    pip install "optimum[onnxruntime]" transformers torch onnxruntime sentencepiece
    python export.py <hf_model_id> <out_dir> [--task text-classification|token-classification] [--trust-remote-code]

Outputs into <out_dir>: model.onnx, the tokenizer files (incl. tokenizer.json),
manifest.json (task/labels/normalization for ort-server), and validation.json
(parity vs the original PyTorch model on fixtures).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

FIXTURES = [
    "My name is John Smith and my SSN is 123-45-6789.",
    "URGENT: verify your account at http://secure-login.example to avoid suspension.",
    "Thanks for the notes from today's standup, talk tomorrow.",
]


def softmax(x: np.ndarray) -> np.ndarray:
    e = np.exp(x - x.max(axis=-1, keepdims=True))
    return e / e.sum(axis=-1, keepdims=True)


def sigmoid(x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-x))


def infer_normalization(config, task: str) -> str:
    """Map HF problem_type semantics to the ort-server manifest contract.

    Regression models have no label scores in [0,1] and are rejected outright;
    multi-label classification needs independent sigmoids, not softmax.
    """
    problem_type = getattr(config, "problem_type", None)
    if problem_type == "regression" or getattr(config, "num_labels", 2) < 2:
        raise SystemExit(
            "unsupported model: regression / single-output heads have no label "
            "scores in [0,1] (problem_type="
            f"{problem_type!r}, num_labels={getattr(config, 'num_labels', None)})"
        )
    if task == "text-classification" and problem_type == "multi_label_classification":
        return "sigmoid"
    return "softmax"


def resolve_max_length(tokenizer, config) -> int:
    """The model's real token budget, so runtime truncation matches validation."""
    n = getattr(tokenizer, "model_max_length", None)
    if not isinstance(n, int) or n < 2 or n > 1_000_000:
        n = getattr(config, "max_position_embeddings", None)
    if not isinstance(n, int) or n < 2 or n > 1_000_000:
        n = 512
    return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("model_id")
    ap.add_argument("out")
    ap.add_argument(
        "--task",
        default="text-classification",
        choices=["text-classification", "token-classification"],
    )
    ap.add_argument("--trust-remote-code", action="store_true")
    args = ap.parse_args()

    import torch
    from transformers import (
        AutoModelForSequenceClassification,
        AutoModelForTokenClassification,
        AutoTokenizer,
    )
    from optimum.onnxruntime import (
        ORTModelForSequenceClassification,
        ORTModelForTokenClassification,
    )
    import onnxruntime as ort

    seq = args.task == "text-classification"
    trc = args.trust_remote_code
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    tokenizer = AutoTokenizer.from_pretrained(args.model_id, trust_remote_code=trc)
    RefCls = (
        AutoModelForSequenceClassification if seq else AutoModelForTokenClassification
    )
    ref = RefCls.from_pretrained(args.model_id, trust_remote_code=trc).eval()
    ORTCls = (
        ORTModelForSequenceClassification if seq else ORTModelForTokenClassification
    )
    ort_model = ORTCls.from_pretrained(
        args.model_id, export=True, trust_remote_code=trc
    )
    ort_model.save_pretrained(out)
    tokenizer.save_pretrained(out)

    normalization = infer_normalization(ref.config, args.task)
    max_length = resolve_max_length(tokenizer, ref.config)
    normalize = sigmoid if normalization == "sigmoid" else softmax
    id2label = {int(k): v for k, v in ref.config.id2label.items()}
    (out / "manifest.json").write_text(
        json.dumps(
            {
                "task": args.task,
                "id2label": id2label,
                "score_normalization": normalization,
                "token_aggregation": None if seq else "max",
                "max_length": max_length,
            },
            indent=2,
        )
    )

    # Validate: ONNX Runtime (CPU) vs the original PyTorch model on fixtures,
    # scored through the SAME normalization ort-server will apply.
    onnx_files = sorted(out.glob("*.onnx"))
    assert (
        len(onnx_files) == 1
    ), f"expected exactly one .onnx in {out}, found {onnx_files}"
    sess = ort.InferenceSession(str(onnx_files[0]), providers=["CPUExecutionProvider"])
    in_names = {i.name for i in sess.get_inputs()}
    max_delta, agree = 0.0, []
    for text in FIXTURES:
        enc_pt = tokenizer(
            text, return_tensors="pt", truncation=True, max_length=max_length
        )
        with torch.no_grad():
            ref_logits = ref(**enc_pt).logits[0].numpy()
        enc_np = tokenizer(
            text, return_tensors="np", truncation=True, max_length=max_length
        )
        onnx_logits = sess.run(
            None, {k: v for k, v in enc_np.items() if k in in_names}
        )[0][0]
        if seq:
            max_delta = max(
                max_delta,
                float(np.abs(normalize(ref_logits) - normalize(onnx_logits)).max()),
            )
        else:
            agree.append(
                float((ref_logits.argmax(-1) == onnx_logits.argmax(-1)).mean())
            )

    validation = {
        "compared_against": args.model_id,
        "fixtures": len(FIXTURES),
        ("max_score_delta" if seq else "token_label_agreement"): (
            round(max_delta, 6) if seq else round(sum(agree) / len(agree), 6)
        ),
    }
    (out / "validation.json").write_text(json.dumps(validation, indent=2))
    print(json.dumps(validation))


if __name__ == "__main__":
    main()
