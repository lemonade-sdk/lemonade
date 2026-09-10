"""
Convert perplexity-ai/pplx-pii-masking (bidirectional Qwen3-0.6B encoder) to
ONNX and run it on CPU with onnxruntime's CPUExecutionProvider.

IMPORTANT - do not export from the companion repo
perplexity-ai/pplx-pii-masking-vllm-tmp instead. That repo repacks the same
weights as a stock transformers Qwen3ForTokenClassification (config.json sets
a top-level "is_causal": false) purely so vanilla vLLM can serve it via a
pooling classify runner. Loading that repacking with plain
AutoModelForTokenClassification is the "easy" path and it is WRONG: stock
transformers' Qwen3Model/Qwen3Attention do not read a top-level "is_causal"
config key at all (see huggingface/transformers#39554) - flash-attn ignores a
passed is_causal=False and only honors each attention module's own
self.is_causal attribute, which stock Qwen3 code never flips, and the causal
4D attention mask itself is built causal by Qwen3Model.forward() regardless
of that config key. This is the same shape of bug as the gpt-oss
reinterpretation documented in privacy_filter_ml_v2_onnx_repro.md: it would
load with the right tensor shapes and look self-consistent, and would be
silently wrong (causal instead of bidirectional attention).

This script instead loads the ORIGINAL repo (perplexity-ai/pplx-pii-masking,
trust_remote_code=True), whose vendored modeling_pplx_qwen3.py gets
bidirectional attention right on purpose: post_init() sets
layer.self_attn.is_causal = False on every layer, AND forward() rebuilds the
attention mask via
create_causal_mask(..., or_mask_function=bidirectional_mask_function(...)) -
a real bidirectional (padding-only) mask, not a causal one wearing a config
flag. There is no MoE and no non-traceable Python loop here (unlike the
gpt-oss-based sibling model) - forward() is plain linear layers, a backbone
call, and a masked mean-pool - so no monkeypatch is needed to make it
traceable.

The model has two heads on top of the shared backbone:
  - token_cls_head (1024 -> 37): per-token BIOES logits over 9 PII types + O.
  - sensitivity_head (1024 -> 1): a sequence-level logit (sigmoid at
    inference) computed over an attention-mask-weighted mean pool of the
    backbone's hidden states.
Both are exported as separate named ONNX outputs ("logits" and
"sensitivity_logits"). BIOES span decoding (the checkpoint's own
ViterbiDecoder) is deliberately NOT part of forward() upstream and is NOT
baked into the ONNX graph either - same choice privacy_filter2onnx.py made -
downstream consumers (Lemonade's manifest-driven token_aggregation="max"
scheme, or a caller's own decoder) work from the raw per-token logits.

Also loads dtype=torch.float32 explicitly, matching privacy_filter2onnx.py:
config.json declares a bf16 default and AutoModel honors it unless
overridden, and bf16 Where-node scalars in the exported graph have no
onnxruntime CPU EP kernel (a NotImplemented error at session-load time, not
export time).
"""

import os
import sys

import numpy as np
import onnxruntime as ort
import torch
from transformers import AutoModel, AutoTokenizer

# torch.onnx's own progress printer emits U+2705 on a successful graph
# capture. On Windows the default cp1252 stdout can't encode it and the
# export dies *after* the graph is already captured, so the failure looks
# like an export bug rather than a console-encoding one. Same reconfigure
# the eval scripts in this directory carry.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

REPO_ID = "perplexity-ai/pplx-pii-masking"
ONNX_OUTPUT_DIR = os.path.join(
    os.path.expanduser("~"),
    ".cache",
    "lemonade-onnx-models",
    "pplx-pii-masking-onnx",
)
os.makedirs(ONNX_OUTPUT_DIR, exist_ok=True)
ONNX_PATH = os.path.join(ONNX_OUTPUT_DIR, "model.onnx")

SAMPLE_TEXT = (
    "Hi, I'm Daniel Whitfield, you can reach me at daniel@meridiancap.com "
    "or 415-555-0123."
)
# Must be reachable so we can decode the checkpoint's own BIOES labels
# without hand-copying them (drift risk) - PII_TYPES is a plain module-level
# list in modeling_pii_masking.py, not something derivable from config.json.
EXPECTED_ENTITY_TYPES = {"private_person", "private_email", "private_phone"}


class LogitsAndSensitivity(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, input_ids, attention_mask):
        out = self.model(input_ids=input_ids, attention_mask=attention_mask)
        return out.logits, out.sensitivity_logits


def main():
    print("Loading tokenizer ...")
    tokenizer = AutoTokenizer.from_pretrained(REPO_ID)

    print("Downloading + loading model weights (fp32, trust_remote_code) ...")
    model = AutoModel.from_pretrained(
        REPO_ID, trust_remote_code=True, dtype=torch.float32
    )
    model.eval()

    modeling_module = sys.modules[type(model).__module__]
    bioes_labels = modeling_module.BIOES_LABELS
    idx_to_label = dict(enumerate(bioes_labels))
    print(f"BIOES label set ({len(bioes_labels)} labels): {bioes_labels}")

    encoded = tokenizer(SAMPLE_TEXT, return_tensors="pt", return_offsets_mapping=True)
    offsets = encoded.pop("offset_mapping")[0]
    input_ids = encoded["input_ids"]
    attention_mask = encoded["attention_mask"]

    print("Running eager PyTorch forward pass (true reference) ...")
    with torch.no_grad():
        true_out = model(input_ids=input_ids, attention_mask=attention_mask)
    true_logits = true_out.logits
    true_sensitivity = true_out.sensitivity_logits
    print("PyTorch logits shape:", tuple(true_logits.shape))
    print("PyTorch sensitivity_logits:", true_sensitivity.item())

    print(
        "Cross-checking against the checkpoint's own .predict() "
        "(real ViterbiDecoder, ground truth for span decoding) ..."
    )
    spans, sensitivity = model.predict(SAMPLE_TEXT, tokenizer=tokenizer)
    found_types = {s.label for s in spans}
    print(f"  predict() spans: {[(s.label, s.start, s.end) for s in spans]}")
    print(f"  predict() sensitivity: {sensitivity}")
    if not EXPECTED_ENTITY_TYPES.issubset(found_types):
        print(
            "ERROR: model.predict() did not find the expected entity types "
            f"{EXPECTED_ENTITY_TYPES} on the sample sentence (found "
            f"{found_types}). Not exporting - this would mean the loaded "
            "model itself is not behaving correctly, independent of ONNX.",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Exporting to ONNX at {ONNX_PATH} ...")
    wrapped = LogitsAndSensitivity(model)
    wrapped.eval()
    with torch.no_grad():
        torch.onnx.export(
            wrapped,
            (input_ids, attention_mask),
            ONNX_PATH,
            input_names=["input_ids", "attention_mask"],
            output_names=["logits", "sensitivity_logits"],
            dynamic_axes={
                "input_ids": {0: "batch", 1: "sequence"},
                "attention_mask": {0: "batch", 1: "sequence"},
                "logits": {0: "batch", 1: "sequence"},
                "sensitivity_logits": {0: "batch"},
            },
            opset_version=18,
        )
    print("Export finished.")
    data_file = ONNX_PATH + ".data"
    if os.path.exists(data_file):
        print(f"External data file written: {data_file}")

    print("Loading exported graph with onnxruntime CPUExecutionProvider ...")
    session = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    print("Active providers:", session.get_providers())

    ort_logits, ort_sensitivity = session.run(
        None,
        {
            "input_ids": input_ids.numpy(),
            "attention_mask": attention_mask.numpy(),
        },
    )

    max_abs_diff_logits = np.max(np.abs(ort_logits - true_logits.numpy()))
    max_abs_diff_sensitivity = np.max(
        np.abs(ort_sensitivity - true_sensitivity.numpy())
    )
    print(f"Max abs diff, ONNX vs. true PyTorch logits: {max_abs_diff_logits:.6f}")
    print(
        "Max abs diff, ONNX vs. true PyTorch sensitivity_logits: "
        f"{max_abs_diff_sensitivity:.6f}"
    )

    if max_abs_diff_logits > 1e-2 or max_abs_diff_sensitivity > 1e-2:
        print(
            "ERROR: ONNX output diverges from the true PyTorch forward pass "
            "by more than 1e-2. Not trusting this export.",
            file=sys.stderr,
        )
        sys.exit(1)

    pred_ids = ort_logits[0].argmax(axis=-1)
    print(f"\nText: {SAMPLE_TEXT}\n\nToken predictions (BIOES, from ONNX logits):")
    onnx_found_types = set()
    for pred_id, (start, end) in zip(pred_ids, offsets):
        if start == end:
            continue
        label = idx_to_label[int(pred_id)]
        if label != "O":
            print(
                f"  {label:<25} [{int(start):>3}:{int(end):<3}] {SAMPLE_TEXT[start:end]!r}"
            )
            onnx_found_types.add(label.split("-", 1)[1])

    if not EXPECTED_ENTITY_TYPES.issubset(onnx_found_types):
        print(
            "ERROR: ONNX argmax predictions did not cover the expected "
            f"entity types {EXPECTED_ENTITY_TYPES} (found {onnx_found_types}). "
            "This means the exported graph is not equivalent to the true "
            "model - do not use this export.",
            file=sys.stderr,
        )
        sys.exit(1)

    onnx_sensitivity_sigmoid = 1.0 / (1.0 + np.exp(-ort_sensitivity[0]))
    print(f"\nONNX sensitivity (sigmoid): {onnx_sensitivity_sigmoid:.4f}")
    print("\nAll success criteria passed.")


if __name__ == "__main__":
    main()
