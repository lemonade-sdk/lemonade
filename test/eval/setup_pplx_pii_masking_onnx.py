"""
Place the pplx-pii-masking ONNX export (produced by
test/eval/pplx_pii_masking2onnx.py) alongside the model's HF-cache snapshot so
Lemonade's onnxruntime backend can serve it, and write the manifest.json that
declares its output contract.

Same registration pattern as test/eval/setup_privacy_filter_onnx.py: /v1/pull's
`checkpoint` field is always validated as an HF repo id (local filesystem
paths are rejected regardless of recipe), so the model gets registered with
checkpoint="perplexity-ai/pplx-pii-masking" (downloading the safetensors,
tokenizer, config, and the vendored trust_remote_code modeling files, which
this script does NOT need - already cached from the earlier standalone
conversion). ort-server then needs model.onnx sitting next to that resolved
snapshot's config.json/tokenizer.json, which is what this script copies
(hardlinked when the cache and export live on the same volume).

A manifest is required: this is a token-classification model (BIOES-tagged
over 9 PII types, 37 labels including "O"), and ort-server's manifest-less
inference defaults to single-label text-classification.

Scope note on the model's second head: the checkpoint also has a
sensitivity_head (a single sequence-level sigmoid score, exported as the
ONNX graph's second output "sensitivity_logits" by pplx_pii_masking2onnx.py).
Per docs/api/lemonade.md, ort-server's /v1/classify manifest schema only
defines two task types today - sequence-classification and
token-classification (aggregated span labels) - there is no "single scalar
sequence score" task type, and the endpoint's response shape (one flat
labels dict) has nowhere to put a second, differently-shaped output. So this
script only wires up the 37-label "logits" output into the manifest, same as
the two sibling PII models already registered this way; "sensitivity_logits"
stays in the .onnx graph unused by Lemonade until/unless a manifest task type
for it exists, rather than inventing a manifest key ort-server doesn't
understand.

Requirements:
    pip install huggingface_hub

Usage:
    python test/eval/setup_pplx_pii_masking_onnx.py [--onnx-dir DIR] [--min-score FLOAT] [--base-url URL]
"""

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

HF_REPO = "perplexity-ai/pplx-pii-masking"
LEMONADE_MODEL_NAME = "user.pplx-pii-masking-onnx"

# Must match modeling_pii_masking.py's PII_TYPES / BIOES_LABELS exactly
# (fetched and verified directly from the repo - see conversation). Kept as a
# literal here, duplicated from pplx_pii_masking2onnx.py's own reconstruction,
# rather than imported, so this script has no dependency on that script's
# runtime environment (trust_remote_code, torch, etc.) succeeding first.
PII_TYPES = [
    "private_person",
    "private_email",
    "private_phone",
    "private_address",
    "private_url",
    "private_date",
    "account_number",
    "secret",
    "other_pii",
]
BIOES_LABELS = ["O"] + [f"{tag}-{t}" for t in PII_TYPES for tag in "BIES"]
assert len(BIOES_LABELS) == 37, f"expected 37 labels, got {len(BIOES_LABELS)}"
ID2LABEL = {str(i): label for i, label in enumerate(BIOES_LABELS)}

MANIFEST = {
    "task": "token-classification",
    "id2label": ID2LABEL,
    "score_normalization": "softmax",
    "token_aggregation": "max",
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--onnx-dir",
        default=str(
            Path.home() / ".cache" / "lemonade-onnx-models" / "pplx-pii-masking-onnx"
        ),
        help="Directory containing model.onnx (+ model.onnx.data if present), "
        "produced by test/eval/pplx_pii_masking2onnx.py",
    )
    p.add_argument(
        "--min-score",
        type=float,
        default=0.5,
        help="Only used for the printed example commands.",
    )
    p.add_argument(
        "--base-url", default="http://localhost:13305", help="Lemonade server base URL"
    )
    return p.parse_args()


def resolve_snapshot_dir() -> Path:
    from huggingface_hub import hf_hub_download

    # config.json is tiny and already cached from pplx_pii_masking2onnx.py's
    # own download - this just resolves the local snapshot path without
    # re-fetching anything.
    config_path = Path(hf_hub_download(repo_id=HF_REPO, filename="config.json"))
    return config_path.parent


def place_onnx_files(onnx_dir: Path, snapshot_dir: Path) -> None:
    src_graph = onnx_dir / "model.onnx"
    if not src_graph.exists():
        print(
            f"ERROR: {src_graph} not found - run pplx_pii_masking2onnx.py first",
            file=sys.stderr,
        )
        sys.exit(1)
    filenames = ["model.onnx"]
    if (onnx_dir / "model.onnx.data").exists():
        filenames.append("model.onnx.data")

    for filename in filenames:
        src = onnx_dir / filename
        dst = snapshot_dir / filename
        if dst.exists():
            dst.unlink()
        try:
            os.link(src, dst)
            print(f"  hardlinked {filename} -> {dst}")
        except OSError:
            shutil.copy2(src, dst)
            print(
                f"  copied {filename} -> {dst} (hardlink unavailable, e.g. cross-volume)"
            )


def write_manifest(snapshot_dir: Path) -> None:
    manifest_path = snapshot_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(MANIFEST, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"Wrote {manifest_path}")


def main() -> None:
    args = parse_args()
    onnx_dir = Path(args.onnx_dir)

    snapshot_dir = resolve_snapshot_dir()
    print(f"Resolved HF cache snapshot: {snapshot_dir}")

    place_onnx_files(onnx_dir, snapshot_dir)
    write_manifest(snapshot_dir)

    required = ["model.onnx", "tokenizer.json", "config.json", "manifest.json"]
    missing = [f for f in required if not (snapshot_dir / f).exists()]
    if missing:
        print(f"ERROR: missing expected files after setup: {missing}", file=sys.stderr)
        sys.exit(1)

    print("\n" + "=" * 70)
    print("Model directory ready:", snapshot_dir)
    print("=" * 70)
    print(f"""
Next steps:

1. lemond must be pointed at a qwen3/pii_masking-capable ort-server build
   (config.json: {{"onnxruntime": {{"cpu_bin": "<path>"}}}}) - the public
   release does not recognize this architecture any more than it recognizes
   modernbert or openai_privacy_filter today (same caveat as
   setup_mmbert32k_onnx.py / setup_privacy_filter_onnx.py).

2. Register the model (checkpoint is the real HF repo id - downloads the
   safetensors/tokenizer/config/trust_remote_code files, already cached,
   alongside the ONNX files just placed above):

     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" -d "{{\\"model_name\\": \\"{LEMONADE_MODEL_NAME}\\", \\"checkpoint\\": \\"{HF_REPO}\\", \\"recipe\\": \\"onnxruntime\\"}}"

3. Generate + register the router policy:

     python test/eval/generate_pplx_pii_masking_onnx_policy.py --min-score {args.min_score}
     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" --data-binary @test/conformance/routing/1/l2_pii_onnx_pplx_masking/policy.json

4. Smoke test, then run the benchmark:

     python test/eval/pii_routing_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --policy ../l2_pii_onnx_pplx_masking/policy.json --limit 20 --verbose
""")


if __name__ == "__main__":
    main()
