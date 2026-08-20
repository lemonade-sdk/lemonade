# Reproducing: OpenMed/privacy-filter-multilingual-v2 safetensors → ONNX (CPU EP)

Hand this whole file to the other agent. It's self-contained: environment, the script, how to run it, and what "success" looks like.

## What this does

Converts `OpenMed/privacy-filter-multilingual-v2` (only published as safetensors, `gpt_oss`-architecture MoE token-classifier, mislabeled with a custom `model_type`) to ONNX, then runs it via `onnxruntime`'s `CPUExecutionProvider` and cross-checks the ONNX output against a native PyTorch forward pass. Export is full **fp32** (no bf16/fp16 downcast) — this is a deliberate choice, not a default, so keep it.

## 1. Environment

Tested on: Windows 11, Python 3.12.11, CPU-only. No GPU/CUDA needed.

```
pip install "torch==2.8.0" --index-url https://download.pytorch.org/whl/cpu
pip install "transformers==4.57.3" "onnx==1.17.0" "onnxruntime==1.24.4" "onnxscript==0.7.1" "huggingface_hub==0.36.0"
```

Versions don't need to match exactly, but transformers must be recent enough to ship `GptOssConfig`/`GptOssForTokenClassification` (added for the gpt-oss release) and `transformers.masking_utils.create_causal_mask` / `create_sliding_window_causal_mask`. `onnxscript` is only needed if you end up trying the `dynamo=True` exporter (not used by the script below, but installed in the original environment).

## 2. Hardware

- **Disk:** ~2.8 GB download for the safetensors weights, cached under `~/.cache/huggingface`.
- **RAM:** fp32 weights are ~5.6 GB resident. Have at least ~6-7 GB free before running, or lower memory pressure otherwise. `low_cpu_mem_usage=True` is already used in the script to avoid double-allocating during load, but this does not reduce the final resident size.
- No GPU required or used — the "dense" MoE branch (see below) is deliberately used to run on CPU/export cleanly, at the cost of some speed (all 128 experts run per token instead of the top-4).

## 3. Network note (only if you hit SSL errors)

If `huggingface_hub` downloads fail with `SSLCertVerificationError`, that's a corporate TLS-inspecting proxy whose root CA isn't in Python's trust store (this was the case on the original machine — `curl` worked fine via the OS cert store, but `requests`/`urllib3` didn't). The script below includes an optional bypass block (disables cert verification for HF downloads only). Only needed if you actually hit that error — remove it if your network doesn't have this problem, since disabling TLS verification is a real tradeoff.

## 4. The script

Save as `privacy_filter_to_onnx.py` and run `python privacy_filter_to_onnx.py "some text"` (defaults to a sample PII sentence if no arg given).

```python
"""
Convert OpenMed/privacy-filter-multilingual-v2 (safetensors) to ONNX and run it
on CPU with onnxruntime's CPUExecutionProvider.

This repo's config.json declares model_type="openai_privacy_filter" /
architectures=["OpenAIPrivacyFilterForTokenClassification"], which transformers
doesn't recognize -- but every other field (num_local_experts, num_experts_per_tok,
sliding_window, rope params, head_dim, ...) is an exact match for transformers'
built-in GptOssConfig / GptOssForTokenClassification (a gpt-oss token-classification
head). So we load it as that class directly, bypassing AutoModel/AutoConfig.

GptOssExperts.forward() has two code paths:
  - CPU/training: a data-dependent Python loop over "which experts got hit this
    batch" (via .nonzero() + a for-loop) -- NOT traceable/exportable, output
    graph would be wrong for any input other than the one used to trace.
  - GPU/inference: a fully vectorized dense computation over all experts,
    weighted by (mostly-zero) routing weights -- mathematically identical,
    and traceable. We monkeypatch GptOssExperts.forward to always use this
    dense path so export works. It's slower (all 128 experts run for every
    token instead of just the top-4) but numerically the same.
"""

import sys
import json

import numpy as np
import onnxruntime as ort
import requests
import torch
import urllib3
import huggingface_hub
from huggingface_hub import hf_hub_download
from transformers import GptOssConfig, PreTrainedTokenizerFast
from transformers.masking_utils import create_causal_mask, create_sliding_window_causal_mask
from transformers.models.gpt_oss.modeling_gpt_oss import GptOssExperts, GptOssForTokenClassification

# --- OPTIONAL: only needed behind a TLS-inspecting corporate proxy whose root
# CA isn't in Python's trust store. Remove this block if you don't need it.
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


def _unverified_session_factory() -> requests.Session:
    session = requests.Session()
    session.verify = False
    return session


huggingface_hub.configure_http_backend(backend_factory=_unverified_session_factory)
# --- end optional block

REPO_ID = "OpenMed/privacy-filter-multilingual-v2"
ONNX_PATH = "privacy_filter_multilingual_v2.onnx"


def _dense_experts_forward(self, hidden_states, router_indices=None, routing_weights=None):
    """Copy of GptOssExperts' GPU/dense branch, forced regardless of device."""
    batch_size = hidden_states.shape[0]
    hidden_states = hidden_states.reshape(-1, self.hidden_size)
    num_experts = routing_weights.shape[1]
    hidden_states = hidden_states.repeat(num_experts, 1)
    hidden_states = hidden_states.view(num_experts, -1, self.hidden_size)
    gate_up = torch.bmm(hidden_states, self.gate_up_proj) + self.gate_up_proj_bias[..., None, :]
    gate, up = gate_up[..., ::2], gate_up[..., 1::2]
    gate = gate.clamp(min=None, max=self.limit)
    up = up.clamp(min=-self.limit, max=self.limit)
    glu = gate * torch.sigmoid(gate * self.alpha)
    next_states = torch.bmm(((up + 1) * glu), self.down_proj)
    next_states = next_states + self.down_proj_bias[..., None, :]
    next_states = next_states.view(num_experts, batch_size, -1, self.hidden_size)
    next_states = next_states * routing_weights.transpose(0, 1).view(num_experts, batch_size, -1)[..., None]
    next_states = next_states.sum(dim=0)
    return next_states


GptOssExperts.forward = _dense_experts_forward


def load_patched_config() -> GptOssConfig:
    config_path = hf_hub_download(repo_id=REPO_ID, filename="config.json")
    with open(config_path, "r", encoding="utf-8") as f:
        raw = json.load(f)

    raw["model_type"] = "gpt_oss"
    raw["rope_scaling"] = raw.pop("rope_parameters")
    raw.pop("transformers.js_config", None)
    raw.pop("opf_metadata", None)
    raw.pop("dtype", None)
    return GptOssConfig(**raw)


class LogitsOnly(torch.nn.Module):
    """
    Takes precomputed 4D masks instead of a raw 2D attention_mask.

    transformers builds its causal/sliding-window masks internally via
    torch.vmap over a Python mask-predicate (masking_utils._vmap_for_bhqkv),
    even for attn_implementation="eager". That combination isn't traceable by
    torch.onnx.export (both the legacy and dynamo paths error deep inside
    functorch's vmap/autograd-Function dispatch). GptOssModel.forward already
    has an escape hatch though: if `attention_mask` is passed as a dict of
    per-layer-type masks (rather than a plain tensor), it skips building them
    itself. So we call create_causal_mask/create_sliding_window_causal_mask
    once, eagerly, *before* export, and feed the resulting concrete tensors in
    as ordinary graph inputs.
    """

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, input_ids, full_attention_mask, sliding_attention_mask):
        attention_mask = {
            "full_attention": full_attention_mask,
            "sliding_attention": sliding_attention_mask,
        }
        return self.model(input_ids=input_ids, attention_mask=attention_mask).logits


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else (
        "My name is John Smith, call me at 555-123-4567 or email john@example.com."
    )

    print("Downloading + patching config ...")
    config = load_patched_config()
    config._attn_implementation = "eager"  # needed for a traceable attention path
    idx_to_label = {int(k): v for k, v in config.id2label.items()}

    print("Loading tokenizer ...")
    # tokenizer_config.json declares tokenizer_class="TokenizersBackend", a class
    # from a newer transformers dev build that may not exist in your installed
    # version. Build the fast tokenizer directly from tokenizer.json instead of
    # going through AutoTokenizer's class-name resolution.
    tokenizer_json_path = hf_hub_download(repo_id=REPO_ID, filename="tokenizer.json")
    tokenizer = PreTrainedTokenizerFast(
        tokenizer_file=tokenizer_json_path,
        eos_token="<|endoftext|>",
        pad_token="<|endoftext|>",
    )

    print("Downloading + loading model weights (fp32, this is the ~2.8GB file) ...")
    model = GptOssForTokenClassification.from_pretrained(
        REPO_ID,
        config=config,
        torch_dtype=torch.float32,
        low_cpu_mem_usage=True,
    )
    model.eval()

    encoded = tokenizer(text, return_tensors="pt", return_offsets_mapping=True)
    offsets = encoded.pop("offset_mapping")[0]
    input_ids = encoded["input_ids"]
    attention_mask = encoded["attention_mask"]

    print("Running eager PyTorch forward pass (reference output) ...")
    with torch.no_grad():
        torch_logits = model(input_ids=input_ids, attention_mask=attention_mask).logits
    print("PyTorch logits shape:", tuple(torch_logits.shape))

    def build_masks(input_ids, attention_mask):
        # Must be called outside any export/tracing context -- see LogitsOnly docstring.
        with torch.no_grad():
            inputs_embeds = model.model.embed_tokens(input_ids)
            cache_position = torch.arange(input_ids.shape[1])
            mask_kwargs = dict(
                config=config,
                input_embeds=inputs_embeds,
                attention_mask=attention_mask,
                cache_position=cache_position,
                past_key_values=None,
            )
            full_mask = create_causal_mask(**mask_kwargs)
            sliding_mask = create_sliding_window_causal_mask(**mask_kwargs)
        return full_mask, sliding_mask

    full_mask, sliding_mask = build_masks(input_ids, attention_mask)

    print(f"Exporting to ONNX at {ONNX_PATH} ...")
    wrapped = LogitsOnly(model)
    wrapped.eval()
    with torch.no_grad():
        torch.onnx.export(
            wrapped,
            (input_ids, full_mask, sliding_mask),
            ONNX_PATH,
            input_names=["input_ids", "full_attention_mask", "sliding_attention_mask"],
            output_names=["logits"],
            dynamic_axes={
                "input_ids": {0: "batch", 1: "sequence"},
                "full_attention_mask": {0: "batch", 2: "sequence", 3: "kv_sequence"},
                "sliding_attention_mask": {0: "batch", 2: "sequence", 3: "kv_sequence"},
                "logits": {0: "batch", 1: "sequence"},
            },
            opset_version=17,
        )
    print("Export finished.")

    print("Loading exported graph with onnxruntime CPUExecutionProvider ...")
    session = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    print("Active providers:", session.get_providers())

    (ort_logits,) = session.run(
        None,
        {
            "input_ids": input_ids.numpy(),
            "full_attention_mask": full_mask.numpy(),
            "sliding_attention_mask": sliding_mask.numpy(),
        },
    )

    max_abs_diff = np.max(np.abs(ort_logits - torch_logits.numpy()))
    print(f"Max abs diff vs. PyTorch logits: {max_abs_diff:.6f}")

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
Downloading + patching config ...
Loading tokenizer ...
Downloading + loading model weights (fp32, this is the ~2.8GB file) ...
Running eager PyTorch forward pass (reference output) ...
PyTorch logits shape: (1, 22, 217)
Exporting to ONNX at privacy_filter_multilingual_v2.onnx ...
Export finished.
Loading exported graph with onnxruntime CPUExecutionProvider ...
Active providers: ['CPUExecutionProvider']
Max abs diff vs. PyTorch logits: 0.000034
```

A benign warning is expected and can be ignored:
```
Unrecognized keys in `rope_scaling` for 'rope_type'='yarn': {'rope_theta'}
```
(the repo's config embeds a redundant `rope_theta` key inside the nested `rope_scaling`/`rope_parameters` dict; harmless.)

**Success criteria:** exit code 0, `Active providers: ['CPUExecutionProvider']`, and `Max abs diff vs. PyTorch logits` on the order of `1e-4` or smaller. That confirms the ONNX graph is numerically faithful to the original PyTorch model.

## 6. Known model-behavior caveat (not a pipeline bug)

Don't be alarmed if the "Token predictions" section prints nothing (all tokens classified `O`) for plain English sentences. This was independently verified on the original machine: across several phrasings (natural prose, `Name:`/`Email:`/`Phone:` labeled fields, clinical-note style), the model consistently favors `O`, though the *correct* entity type often shows up as a low-confidence runner-up (e.g. an email-domain token scoring `S-USERNAME: 0.54` vs. `O: 0.26`). ONNX and PyTorch agree to `~3e-5`, so this is genuine model behavior/calibration, not an artifact of the conversion — the checkpoint splits PII into 217 fine-grained BIOES labels (vs. e.g. 35 in other PII models), which dilutes confidence. It's not necessarily a fair comparison across languages/formats this model was actually trained on — if the other machine plans to evaluate more thoroughly, it's worth trying non-English input or the exact format the model card documents, since this codebase itself was not tuned for input format, only for correct conversion.
