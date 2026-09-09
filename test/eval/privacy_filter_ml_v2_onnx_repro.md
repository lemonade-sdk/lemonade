# Reproducing: OpenMed/privacy-filter-multilingual-v2 safetensors → ONNX (CPU EP)

Hand this whole file to the other agent. It's self-contained: environment, the script, how to run it, and what "success" looks like.

## What this does

Converts `OpenMed/privacy-filter-multilingual-v2` (only published as safetensors) to ONNX, then runs it via `onnxruntime`'s `CPUExecutionProvider` and cross-checks the ONNX output against the **unpatched** native PyTorch forward pass. Export is full **fp32** (no bf16/fp16 downcast) — this is required, not cosmetic; see §7.

The checkpoint declares `model_type="openai_privacy_filter"` / `architectures=["OpenAIPrivacyFilterForTokenClassification"]`. That is a **genuine, first-class transformers architecture** as of transformers 5.x — load it with plain `AutoModelForTokenClassification`. There is no config to hand-patch and no masking workaround needed.

> **Do not reinterpret this checkpoint as gpt-oss.** An earlier version of this guide loaded it as `GptOssConfig` / `GptOssForTokenClassification`, because transformers 4.57.3 didn't recognize `openai_privacy_filter`. Every tensor shape matches and it loads with zero missing/unexpected keys, so the export looked healthy — ONNX agreed with its own PyTorch reference to ~2e-5 — but the model is **wrong**: gpt-oss is causal/decoder-style, this model is bidirectional with sliding-window attention (`create_bidirectional_sliding_window_mask`) and a single mask shared across all layers. The symptom was that it predicted almost all `O` on obvious PII. Numerics alone cannot catch this; only comparing against the correct native class does.

## 1. Environment

Tested on: Windows 11, Python 3.12.10, CPU-only. No GPU/CUDA needed.

```
pip install "torch==2.13.0" --index-url https://download.pytorch.org/whl/cpu
pip install "transformers==5.11.0" "onnx==1.22.0" "onnxruntime==1.28.0" "onnxscript==0.7.2" "huggingface_hub==1.28.0" "numpy==1.26.4"
```

Versions don't need to match exactly, but there are two hard floors:

- **transformers must be 5.x or newer** — it has to ship `transformers.models.openai_privacy_filter`. On 4.x, `AutoModelForTokenClassification` will fail to resolve the architecture and you will be tempted into the gpt-oss reinterpretation described above. Don't.
- **torch must be new enough that `torch.onnx.export` uses the dynamo exporter by default** (2.9+; 2.13.0 tested). `onnxscript` is a real dependency of that path, not optional.

## 2. Hardware

- **Disk:** ~2.8 GB for the safetensors download (cached under `~/.cache/huggingface`), **plus ~5.6 GB for the exported ONNX** written to the output directory below. Budget ~9 GB total.
- **RAM:** fp32 weights are ~5.6 GB resident, and export holds the traced graph alongside them. Have at least ~12 GB free.
- No GPU required or used — the dense MoE branch (see §4) is deliberately used so the graph traces cleanly, at the cost of some speed (all experts run per token instead of just the top-k).

## 3. Output location

The script writes to a stable directory outside the repo:

```
~/.cache/lemonade-onnx-models/privacy-filter-ml-v2-onnx/
    model.onnx        (~2.4 MB — graph only)
    model.onnx.data   (~5.6 GB — externalized initializers)
```

fp32 weights exceed the 2 GB protobuf hard limit, so `torch.onnx.export` splits large initializers into a companion `.data` file. **Both files matter and must stay side by side** — `model.onnx` alone is useless. This is also why the script does not write into the current working directory.

## 4. The script

Already checked in as `privacy_filter2onnx.py`. Run `python privacy_filter2onnx.py "some text"` (defaults to a sample PII sentence if no arg given).

```python
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
```

## 5. Expected output

```
Loading tokenizer ...
Downloading + loading model weights (fp32, this is the ~2.8GB file) ...
Running eager PyTorch forward pass (true, unpatched reference) ...
PyTorch logits shape: (1, 22, 217)
Running eager PyTorch forward pass (dense-patched, traceable reference) ...
Max abs diff, dense-patched vs. true unpatched forward: 0.000013
Exporting to ONNX at C:\Users\...\.cache\lemonade-onnx-models\privacy-filter-ml-v2-onnx\model.onnx ...
[torch.onnx] Obtain model graph for `LogitsOnly([...]` with `torch.export.export(..., strict=False)`...
[torch.onnx] Obtain model graph for `LogitsOnly([...]` with `torch.export.export(..., strict=False)`... ✅
[torch.onnx] Run decompositions...
[torch.onnx] Run decompositions... ✅
[torch.onnx] Translate the graph into ONNX...
[torch.onnx] Translate the graph into ONNX... ✅
[torch.onnx] Optimize the ONNX graph...
[torch.onnx] Optimize the ONNX graph... ✅
Export finished.
Loading exported graph with onnxruntime CPUExecutionProvider ...
Active providers: ['CPUExecutionProvider']
Max abs diff, ONNX vs. true unpatched PyTorch logits: 0.000018

Text: My name is John Smith, call me at 555-123-4567 or email john@example.com.

Token predictions (BIOES):
  S-FIRSTNAME          [ 10:15 ] ' John'
  S-LASTNAME           [ 15:21 ] ' Smith'
  B-PHONE              [ 34:37 ] '555'
  I-PHONE              [ 37:38 ] '-'
  I-PHONE              [ 38:41 ] '123'
  I-PHONE              [ 41:42 ] '-'
  I-PHONE              [ 42:45 ] '456'
  E-PHONE              [ 45:46 ] '7'
  B-EMAIL              [ 55:60 ] ' john'
  I-EMAIL              [ 60:68 ] '@example'
  E-EMAIL              [ 68:72 ] '.com'
```

### Success criteria

All four must hold:

1. Exit code 0.
2. `Max abs diff, dense-patched vs. true unpatched forward` on the order of `1e-4` or smaller. This is the load-bearing check — it proves the dense-experts monkeypatch reproduces the real model. The script hard-fails (`exit 1`) above `1e-2`.
3. `Active providers: ['CPUExecutionProvider']` and `Max abs diff, ONNX vs. true unpatched PyTorch logits` on the order of `1e-4` or smaller.
4. **The token predictions are non-empty and correct.** For the default sentence you must see `S-FIRSTNAME`/`S-LASTNAME` on John Smith, a `B-/I-/E-PHONE` span on the phone number, and a `B-/I-/E-EMAIL` span on the address.

Criterion 4 is not decoration. Criteria 2 and 3 are self-consistency checks and both passed comfortably while the model was being loaded under the wrong architecture. An all-`O` output means the architecture or the experts monkeypatch is wrong — it is **not** a model-calibration quirk.

### Benign warnings

These are expected on stderr and can be ignored:

```
Warning: You are sending unauthenticated requests to the HF Hub. Please set a HF_TOKEN ...
UserWarning: # 'dynamic_axes' is not recommended when dynamo=True, and may lead to
  'torch._dynamo.exc.UserError: Constraints violated.' Supply the 'dynamic_shapes' argument instead ...
UserWarning: # The axis name: sequence will not be used, since it shares the same shape
  constraints with another axis: sequence.
FutureWarning: `isinstance(treespec, LeafSpec)` is deprecated ...
```

## 6. Why the experts monkeypatch is needed, and the two ways to get it wrong

`OpenAIPrivacyFilterExperts.forward()` takes a data-dependent path — `.nonzero()` plus a Python loop over "which experts got hit this batch" — that is not traceable. The exported graph would be baked to whichever experts happened to fire on the tracing input. The patch replaces it with the mathematically identical dense computation over all experts, weighted by mostly-zero routing weights.

Two details in that reconstruction are easy to get wrong, and both produce a **self-consistent but completely incorrect** export (ONNX matches its own wrong PyTorch reference to ~1e-5, predictions collapse to all-`O`):

- **Gate/up split.** This model uses `gate, up = gate_up.chunk(2, dim=-1)` — concatenated halves, per its own `_apply_gate`. gpt-oss uses interleaved `gate_up[..., ::2]` / `[..., 1::2]`. Using the interleaved form here is wrong.
- **Routing weights are sparse.** The native forward passes `(router_indices, routing_weights)` with shape `(num_tokens, top_k)`, *not* a dense `(num_tokens, num_experts)` tensor. They must be scattered into a dense matrix before the per-expert weighting.

The `patch_diff` check in the script (success criterion 2) exists specifically to catch both. If you adapt this script to a different checkpoint, re-verify the split and keep that check.

## 7. Why fp32 is mandatory

`config.json` declares a bf16 default and `AutoModel` honors it unless overridden. bf16 weights produce bf16 scalars on `Where` nodes in the exported graph, and onnxruntime's CPU EP has no kernel for them. The failure is a `NotImplemented` error at **session-load time**, not at export time — so the export appears to succeed and then the model won't run. Hence the explicit `dtype=torch.float32`.

## 8. Known nit

`import json` in the script is dead — a leftover from the removed hand-rolled config patcher. Harmless.
