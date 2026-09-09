"""
Compare a PII model's safetensors (PyTorch, eager, ground truth) output
against its already-exported ONNX Runtime output, for arbitrary input text.

Generalized over the two model families converted in this directory:
  - pplx-pii-masking     (perplexity-ai/pplx-pii-masking, bidirectional
                           Qwen3-0.6B; see pplx_pii_masking2onnx.py)
  - privacy-filter-ml-v2 (OpenMed/privacy-filter-multilingual-v2, gpt-oss-style
                           MoE; see privacy_filter2onnx.py)

This is a read-only comparison tool - it does not export ONNX itself (run the
matching *2onnx.py script first) and does not touch Lemonade's registered
models. To add a third model, add one entry to MODEL_REGISTRY with a loader
function that returns (forward_fn, idx_to_label); everything else (arg
parsing, tokenization, diffing, span printing) is shared.

Run inside the same isolated venv used for the model's own *2onnx.py export
(pinned torch/transformers/onnxruntime versions matter - see that script's
docstring), e.g.:

    <venv>/Scripts/python.exe test/eval/compare_pii_onnx_vs_safetensors.py \\
        --model pplx-pii-masking --text "call me at 555-123-4567"

Usage:
    python test/eval/compare_pii_onnx_vs_safetensors.py --model {pplx-pii-masking,privacy-filter-ml-v2} [--text TEXT] [--onnx-path FILE] [--gate FLOAT]
"""

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Tuple

import numpy as np
import onnxruntime as ort
import torch

DEFAULT_TEXT = (
    "Hi, I'm Daniel Whitfield, you can reach me at daniel@meridiancap.com "
    "or 415-555-0123."
)
DEFAULT_GATE = 1e-2


@dataclass
class ModelSpec:
    repo_id: str
    onnx_dir_name: str  # under ~/.cache/lemonade-onnx-models/<name>/model.onnx
    output_names: Tuple[str, ...]  # ONNX graph output names, in order
    load_safetensors: Callable[[str], tuple]  # repo_id -> (forward_fn, idx_to_label)


def _load_pplx_pii_masking(repo_id: str):
    from transformers import AutoModel

    model = AutoModel.from_pretrained(
        repo_id, trust_remote_code=True, dtype=torch.float32
    )
    model.eval()

    # BIOES_LABELS lives as a plain module-level list in the checkpoint's own
    # vendored modeling_pii_masking.py - reading it off the loaded module
    # avoids a hand-copied label list drifting from the real one.
    modeling_module = sys.modules[type(model).__module__]
    idx_to_label = dict(enumerate(modeling_module.BIOES_LABELS))

    def forward(input_ids, attention_mask):
        with torch.no_grad():
            out = model(input_ids=input_ids, attention_mask=attention_mask)
        return (out.logits, out.sensitivity_logits)

    return forward, idx_to_label


def _load_privacy_filter_ml_v2(repo_id: str):
    from transformers import AutoModelForTokenClassification
    from transformers.models.openai_privacy_filter.modeling_openai_privacy_filter import (
        OpenAIPrivacyFilterExperts,
    )

    model = AutoModelForTokenClassification.from_pretrained(
        repo_id, dtype=torch.float32
    )
    model.eval()
    idx_to_label = {int(k): v for k, v in model.config.id2label.items()}

    # Same dense-MoE monkeypatch as privacy_filter2onnx.py, duplicated (not
    # imported) so this comparison tool has no import-path dependency on that
    # script. See that script's docstring for why the native per-expert
    # Python loop isn't traceable, and for the two bugs this exact
    # reconstruction had to get right (sparse->dense routing-weight scatter
    # direction; concatenated, not interleaved, gate/up split) - re-verify
    # both if you ever point this at a different checkpoint.
    def _dense_experts_forward(
        self, hidden_states, router_indices=None, routing_weights=None
    ):
        num_tokens = hidden_states.shape[0]
        num_experts = self.num_experts
        dense_weights = hidden_states.new_zeros(num_tokens, num_experts)
        dense_weights.scatter_(
            1, router_indices, routing_weights.to(dense_weights.dtype)
        )
        hidden_states = hidden_states.repeat(num_experts, 1)
        hidden_states = hidden_states.view(num_experts, num_tokens, self.hidden_size)
        gate_up = (
            torch.bmm(hidden_states, self.gate_up_proj)
            + self.gate_up_proj_bias[..., None, :]
        )
        gate, up = gate_up.chunk(2, dim=-1)
        gate = gate.clamp(min=None, max=self.limit)
        up = up.clamp(min=-self.limit, max=self.limit)
        glu = gate * torch.sigmoid(gate * self.alpha)
        next_states = torch.bmm(((up + 1) * glu), self.down_proj)
        next_states = next_states + self.down_proj_bias[..., None, :]
        next_states = next_states.view(num_experts, num_tokens, self.hidden_size)
        next_states = next_states * dense_weights.transpose(0, 1)[..., None]
        return next_states.sum(dim=0)

    OpenAIPrivacyFilterExperts.forward = _dense_experts_forward

    def forward(input_ids, attention_mask):
        with torch.no_grad():
            out = model(input_ids=input_ids, attention_mask=attention_mask)
        return (out.logits,)

    return forward, idx_to_label


MODEL_REGISTRY: Dict[str, ModelSpec] = {
    "pplx-pii-masking": ModelSpec(
        repo_id="perplexity-ai/pplx-pii-masking",
        onnx_dir_name="pplx-pii-masking-onnx",
        output_names=("logits", "sensitivity_logits"),
        load_safetensors=_load_pplx_pii_masking,
    ),
    "privacy-filter-ml-v2": ModelSpec(
        repo_id="OpenMed/privacy-filter-multilingual-v2",
        onnx_dir_name="privacy-filter-ml-v2-onnx",
        output_names=("logits",),
        load_safetensors=_load_privacy_filter_ml_v2,
    ),
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--model",
        required=True,
        choices=sorted(MODEL_REGISTRY),
        help="Which registered model family to compare.",
    )
    p.add_argument(
        "--text", default=DEFAULT_TEXT, help="Input text to run through both versions."
    )
    p.add_argument(
        "--onnx-path",
        default=None,
        help="Override path to model.onnx (defaults to "
        "~/.cache/lemonade-onnx-models/<name>/model.onnx).",
    )
    p.add_argument(
        "--gate",
        type=float,
        default=DEFAULT_GATE,
        help="Max-abs-diff threshold above which the comparison is reported FAIL.",
    )
    return p.parse_args()


def print_spans(source_label, offsets, text, idx_to_label, pred_ids) -> set:
    print(f"\n[{source_label}] token predictions (non-'O' only):")
    found_types = set()
    any_shown = False
    for pred_id, (start, end) in zip(pred_ids, offsets):
        if start == end:
            continue
        tag = idx_to_label[int(pred_id)]
        if tag != "O":
            any_shown = True
            found_types.add(tag.split("-", 1)[-1])
            print(f"  {tag:<25} [{int(start):>3}:{int(end):<3}] {text[start:end]!r}")
    if not any_shown:
        print("  (none)")
    return found_types


def main() -> None:
    args = parse_args()
    spec = MODEL_REGISTRY[args.model]

    onnx_path = (
        Path(args.onnx_path)
        if args.onnx_path
        else (
            Path.home()
            / ".cache"
            / "lemonade-onnx-models"
            / spec.onnx_dir_name
            / "model.onnx"
        )
    )
    if not onnx_path.exists():
        print(
            f"ERROR: {onnx_path} not found - export it first with the matching "
            f"*2onnx.py script for {spec.repo_id}, or pass --onnx-path.",
            file=sys.stderr,
        )
        sys.exit(1)

    from transformers import AutoTokenizer

    print(f"Loading tokenizer for {spec.repo_id} ...")
    tokenizer = AutoTokenizer.from_pretrained(spec.repo_id)

    print(f"Loading safetensors model {spec.repo_id} (fp32) ...")
    forward_fn, idx_to_label = spec.load_safetensors(spec.repo_id)

    encoded = tokenizer(args.text, return_tensors="pt", return_offsets_mapping=True)
    offsets = encoded.pop("offset_mapping")[0]
    input_ids = encoded["input_ids"]
    attention_mask = encoded["attention_mask"]

    print("Running safetensors (PyTorch eager) forward pass ...")
    torch_outputs = forward_fn(input_ids, attention_mask)

    print(f"Loading ONNX graph from {onnx_path} ...")
    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    print("Active providers:", session.get_providers())
    onnx_outputs = session.run(
        list(spec.output_names),
        {"input_ids": input_ids.numpy(), "attention_mask": attention_mask.numpy()},
    )

    print(f"\nText: {args.text!r}")

    overall_pass = True
    for name, torch_out, onnx_out in zip(
        spec.output_names, torch_outputs, onnx_outputs
    ):
        diff = float(np.max(np.abs(onnx_out - torch_out.numpy())))
        status = "OK" if diff <= args.gate else "FAIL"
        overall_pass = overall_pass and (status == "OK")
        print(
            f"[{name}] max abs diff (ONNX vs. safetensors): {diff:.6f} "
            f"[{status}, gate={args.gate}]"
        )

    logits_idx = spec.output_names.index("logits")
    torch_pred_ids = torch_outputs[logits_idx][0].argmax(dim=-1).numpy()
    onnx_pred_ids = onnx_outputs[logits_idx][0].argmax(axis=-1)
    torch_types = print_spans(
        "safetensors", offsets, args.text, idx_to_label, torch_pred_ids
    )
    onnx_types = print_spans("onnx", offsets, args.text, idx_to_label, onnx_pred_ids)
    if torch_types != onnx_types:
        print(
            f"\nWARNING: entity types differ between safetensors ({torch_types}) "
            f"and ONNX ({onnx_types}) argmax predictions."
        )
        overall_pass = False

    print(f"\n{'PASS' if overall_pass else 'FAIL'} (gate={args.gate})")
    sys.exit(0 if overall_pass else 1)


if __name__ == "__main__":
    main()
