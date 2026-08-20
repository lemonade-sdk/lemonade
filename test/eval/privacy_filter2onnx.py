"""
Convert OpenMed/privacy-filter-multilingual-v2 (safetensors) to ONNX and run it
on CPU with onnxruntime's CPUExecutionProvider.

IMPORTANT - superseded approach, corrected in place (see conversation): this
script originally reinterpreted the checkpoint as transformers' generic
GptOssConfig/GptOssForTokenClassification, because config.json declares
model_type="openai_privacy_filter", which older transformers releases (the
repro guide was written against 4.57.3) didn't recognize as a distinct
architecture. The transformers version actually installed here (5.11.0) has
since shipped a genuine first-class implementation --
transformers.models.openai_privacy_filter.modeling_openai_privacy_filter.OpenAIPrivacyFilterForTokenClassification,
reachable through plain AutoModelForTokenClassification -- and it is NOT
equivalent to the gpt-oss reinterpretation: gpt-oss is causal/decoder-style,
this model is bidirectional with sliding-window attention
(create_bidirectional_sliding_window_mask), a single mask shared across all
layers (no separate full/sliding mask pair). The gpt-oss reinterpretation
loaded with zero missing/unexpected weight keys (same tensor shapes) and was
internally self-consistent (ONNX matched its own from-that-same-reinterpretation
PyTorch reference to ~2e-5), which is exactly why the wrong-architecture bug
was invisible from numerics alone -- it surfaced only by comparing against the
correct native class's predictions on a sample sentence (John Smith / phone /
email correctly tagged natively; the gpt-oss reinterpretation predicted
almost all "O"). Use AutoModel* directly; there's no need to hand-roll a
config or a masking workaround at all.

GptOssExperts-style modules (OpenAIPrivacyFilterExperts here) still have the
same two forward() code paths:
  - CPU/training: a data-dependent Python loop over "which experts got hit
    this batch" (.nonzero() + a for-loop) -- NOT traceable/exportable.
  - A fully vectorized dense computation over all experts, weighted by
    (mostly-zero) routing weights -- mathematically identical, and
    traceable. We monkeypatch OpenAIPrivacyFilterExperts.forward to always
    use this dense path, reconstructing the dense per-expert weight matrix
    from the sparse (router_indices, routing_weights) pair the native
    forward passes (shape (num_tokens, top_k), NOT a dense
    (num_tokens, num_experts) tensor) via scatter_.

  Verified this model's gate/up split is "gate, up = gate_up.chunk(2, dim=-1)"
  (concatenated halves, per this class's own _apply_gate) -- NOT gpt-oss's
  interleaved gate_up[..., ::2]/[..., 1::2]. Getting this wrong produced a
  self-consistent-looking export (ONNX matched ITS OWN wrong PyTorch
  reference to ~1e-5) that was still completely incorrect relative to the
  true model (predicted almost all "O"); only comparing against the
  unpatched native forward on a known example caught it. Re-verify this
  split (and re-run the true-vs-patched diff check below) if you ever adapt
  this script to a different checkpoint.

Also loads dtype=torch.float32 explicitly: config.json declares a bf16
default, and AutoModel defaults to it if not overridden, which produces
bf16 Where-node scalars in the exported graph that onnxruntime's CPU EP
doesn't have a kernel for (NotImplemented error at session-load time, not at
export time).
"""

import os
import sys
import json

import numpy as np
import onnxruntime as ort
import torch
from transformers import AutoModelForTokenClassification, AutoTokenizer

REPO_ID = "OpenMed/privacy-filter-multilingual-v2"
# fp32 weights are ~5.6GB -- over the 2GB protobuf hard limit, so
# torch.onnx.export externalizes large initializers into companion file(s)
# next to ONNX_PATH. Both the graph file AND those companion files matter;
# a stable directory outside the repo keeps that pairing intact instead of
# scattering multi-GB binaries into whatever the cwd happens to be.
ONNX_OUTPUT_DIR = os.path.join(
    os.path.expanduser("~"),
    ".cache",
    "lemonade-onnx-models",
    "privacy-filter-ml-v2-onnx",
)
os.makedirs(ONNX_OUTPUT_DIR, exist_ok=True)
ONNX_PATH = os.path.join(ONNX_OUTPUT_DIR, "model.onnx")


def _dense_experts_forward(
    self, hidden_states, router_indices=None, routing_weights=None
):
    """Traceable replacement for OpenAIPrivacyFilterExperts.forward's native
    per-expert Python loop (see module docstring for why, and for the two
    bugs this exact reconstruction had to get right: the sparse->dense
    routing-weight scatter, and the concatenated (not interleaved) gate/up
    split)."""
    num_tokens = hidden_states.shape[0]
    num_experts = self.num_experts
    dense_weights = hidden_states.new_zeros(num_tokens, num_experts)
    dense_weights.scatter_(1, router_indices, routing_weights.to(dense_weights.dtype))

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
    next_states = next_states.sum(dim=0)
    return next_states


class LogitsOnly(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, input_ids, attention_mask):
        return self.model(input_ids=input_ids, attention_mask=attention_mask).logits


def main():
    text = (
        sys.argv[1]
        if len(sys.argv) > 1
        else (
            "My name is John Smith, call me at 555-123-4567 or email john@example.com."
        )
    )

    # Patch before load: from_pretrained's module tree already contains the
    # OpenAIPrivacyFilterExperts instances the monkeypatch needs to affect.
    from transformers.models.openai_privacy_filter.modeling_openai_privacy_filter import (
        OpenAIPrivacyFilterExperts,
    )

    print("Loading tokenizer ...")
    tokenizer = AutoTokenizer.from_pretrained(REPO_ID)

    print("Downloading + loading model weights (fp32, this is the ~2.8GB file) ...")
    model = AutoModelForTokenClassification.from_pretrained(
        REPO_ID, dtype=torch.float32
    )
    model.eval()
    idx_to_label = {int(k): v for k, v in model.config.id2label.items()}

    encoded = tokenizer(text, return_tensors="pt", return_offsets_mapping=True)
    offsets = encoded.pop("offset_mapping")[0]
    input_ids = encoded["input_ids"]
    attention_mask = encoded["attention_mask"]

    print("Running eager PyTorch forward pass (true, unpatched reference) ...")
    with torch.no_grad():
        true_logits = model(input_ids=input_ids, attention_mask=attention_mask).logits
    print("PyTorch logits shape:", tuple(true_logits.shape))

    OpenAIPrivacyFilterExperts.forward = _dense_experts_forward

    print("Running eager PyTorch forward pass (dense-patched, traceable reference) ...")
    with torch.no_grad():
        torch_logits = model(input_ids=input_ids, attention_mask=attention_mask).logits
    patch_diff = (torch_logits - true_logits).abs().max().item()
    print(f"Max abs diff, dense-patched vs. true unpatched forward: {patch_diff:.6f}")
    if patch_diff > 1e-2:
        print(
            "ERROR: dense-patched forward diverges from the true model -- the "
            "monkeypatch is wrong for this checkpoint. Not exporting. See "
            "module docstring for the two bugs this exact reconstruction had "
            "to get right before trusting it.",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Exporting to ONNX at {ONNX_PATH} ...")
    wrapped = LogitsOnly(model)
    wrapped.eval()
    with torch.no_grad():
        torch.onnx.export(
            wrapped,
            (input_ids, attention_mask),
            ONNX_PATH,
            input_names=["input_ids", "attention_mask"],
            output_names=["logits"],
            dynamic_axes={
                "input_ids": {0: "batch", 1: "sequence"},
                "attention_mask": {0: "batch", 1: "sequence"},
                "logits": {0: "batch", 1: "sequence"},
            },
            opset_version=18,
        )
    print("Export finished.")

    print("Loading exported graph with onnxruntime CPUExecutionProvider ...")
    session = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    print("Active providers:", session.get_providers())

    (ort_logits,) = session.run(
        None,
        {
            "input_ids": input_ids.numpy(),
            "attention_mask": attention_mask.numpy(),
        },
    )

    max_abs_diff = np.max(np.abs(ort_logits - true_logits.numpy()))
    print(f"Max abs diff, ONNX vs. true unpatched PyTorch logits: {max_abs_diff:.6f}")

    pred_ids = ort_logits[0].argmax(axis=-1)
    print(f"\nText: {text}\n\nToken predictions (BIOES):")
    for pred_id, (start, end) in zip(pred_ids, offsets):
        if start == end:
            continue
        label = idx_to_label[int(pred_id)]
        if label != "O":
            print(f"  {label:<20} [{int(start):>3}:{int(end):<3}] {text[start:end]!r}")


if __name__ == "__main__":
    main()
