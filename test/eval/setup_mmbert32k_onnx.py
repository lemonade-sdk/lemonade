"""
Download the mmbert32k-pii-detector ONNX export and prepare it for Lemonade's
onnxruntime backend (ort-server: https://github.com/lemonade-sdk/ort-server).

Downloads only the files needed to serve the model from
llm-semantic-router/mmbert32k-pii-detector-merged's onnx/ subfolder - not the
whole HF repo, which also carries a ~1.2GB safetensors checkpoint and two
extra fp16 ONNX variants we don't need - into a local directory, then writes a
manifest.json declaring the output contract explicitly.

A manifest is required here: this is a token-classification model (BIO-tagged
over 17 entity types, 35 labels including "O"), and ort-server's manifest-less
inference defaults to single-label text-classification (see ort-server's
README), which would misinterpret this model's [batch, tokens, 35] output
shape and either error or silently score wrong.

The manifest deliberately keeps all 35 raw BIO labels distinct
("B-EMAIL_ADDRESS", "I-EMAIL_ADDRESS", ...) rather than collapsing B-/I- pairs
into one bare entity name. Traced directly in ort-server's main.cpp: per-label
scores land in a std::map<string, float> keyed by the label string via plain
assignment (scores[id2label[l]] = ...), not accumulation - two label indices
sharing one name would have the later index's score silently overwrite the
earlier one's, instead of the two being combined. The router policy's match
rule works around this by checking "does the B- OR the I- variant cross
threshold" per entity type, rather than relying on a collapsed label.

Requirements:
    pip install huggingface_hub

Usage:
    python test/eval/setup_mmbert32k_onnx.py [--output-dir DIR] [--min-score FLOAT]

Defaults:
    --output-dir  <scratch dir outside the repo - see --help for the exact
                  default, or pass your own>
    --min-score   0.5 (only used to print the ready-to-run curl/pull commands;
                  does not affect the downloaded files)

After running, this script prints the exact commands to:
  1. point lemond at a modernbert-capable ort-server build (config.json
     override - the public 0.3.7 release does not support modernbert yet)
  2. register the model (POST /v1/pull with a local-path checkpoint)
  3. run the benchmark (reuses pii_routing_eval.py unmodified, against
     test/conformance/routing/1/l2_pii_onnx_classifier/policy.json)
"""

import argparse
import json
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

HF_REPO = "llm-semantic-router/mmbert32k-pii-detector-merged"
# The files ort-server actually needs (model.onnx + tokenizer.json + config.json
# are mandatory; tokenizer_config.json supplies model_max_length). Deliberately
# NOT downloading model_fa_fp16.onnx / model_sdpa_fp16.onnx (ort-server rejects
# non-float32 outputs, and it's an unverified guess whether those variants'
# output layer is still float32) or model.safetensors (PyTorch checkpoint,
# unused by ort-server) or the repo-root copies (identical content, onnx/ has
# its own self-contained set already).
NEEDED_FILES = [
    "onnx/model.onnx",
    "onnx/tokenizer.json",
    "onnx/config.json",
    "onnx/tokenizer_config.json",
]

# From onnx/config.json's id2label, fetched and verified directly (see
# conversation) - kept as a literal here rather than re-derived at manifest
# -write time so this script has no dependency on the download succeeding
# in a specific order, and so the manifest is correct even if config.json's
# own copy ever drifts from what the model.onnx graph was actually exported
# with.
ID2LABEL = {
    "0": "O",
    "1": "B-AGE",
    "2": "I-AGE",
    "3": "B-CREDIT_CARD",
    "4": "I-CREDIT_CARD",
    "5": "B-DATE_TIME",
    "6": "I-DATE_TIME",
    "7": "B-DOMAIN_NAME",
    "8": "I-DOMAIN_NAME",
    "9": "B-EMAIL_ADDRESS",
    "10": "I-EMAIL_ADDRESS",
    "11": "B-GPE",
    "12": "I-GPE",
    "13": "B-IBAN_CODE",
    "14": "I-IBAN_CODE",
    "15": "B-IP_ADDRESS",
    "16": "I-IP_ADDRESS",
    "17": "B-NRP",
    "18": "I-NRP",
    "19": "B-ORGANIZATION",
    "20": "I-ORGANIZATION",
    "21": "B-PERSON",
    "22": "I-PERSON",
    "23": "B-PHONE_NUMBER",
    "24": "I-PHONE_NUMBER",
    "25": "B-STREET_ADDRESS",
    "26": "I-STREET_ADDRESS",
    "27": "B-TITLE",
    "28": "I-TITLE",
    "29": "B-US_DRIVER_LICENSE",
    "30": "I-US_DRIVER_LICENSE",
    "31": "B-US_SSN",
    "32": "I-US_SSN",
    "33": "B-ZIP_CODE",
    "34": "I-ZIP_CODE",
}

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
        "--output-dir",
        default=str(
            Path.home() / ".cache" / "lemonade-onnx-models" / "mmbert32k-pii-onnx"
        ),
        help="Local directory to download into and register as the model's "
        "checkpoint. Deliberately outside this repo - it's a ~1.2GB binary, "
        "not something to commit.",
    )
    p.add_argument(
        "--min-score",
        type=float,
        default=0.5,
        help="Only used for the printed example commands (matches the "
        "threshold already baked into policy.json's match rule).",
    )
    p.add_argument(
        "--base-url", default="http://localhost:13305", help="Lemonade server base URL"
    )
    return p.parse_args()


def download(output_dir: Path) -> None:
    from huggingface_hub import hf_hub_download

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {len(NEEDED_FILES)} files from {HF_REPO} into {output_dir} ...")
    for repo_file in NEEDED_FILES:
        local_name = Path(repo_file).name
        print(f"  {repo_file} -> {local_name}")
        downloaded = hf_hub_download(
            repo_id=HF_REPO, filename=repo_file, local_dir=str(output_dir)
        )
        # hf_hub_download preserves the repo's onnx/ subdirectory under
        # local_dir; flatten it so the model-dir contract (model.onnx,
        # tokenizer.json, config.json, manifest.json all siblings) holds.
        downloaded_path = Path(downloaded)
        target = output_dir / local_name
        if downloaded_path != target:
            target.write_bytes(downloaded_path.read_bytes())
    onnx_subdir = output_dir / "onnx"
    if onnx_subdir.exists() and onnx_subdir.is_dir():
        import shutil

        shutil.rmtree(onnx_subdir, ignore_errors=True)


def write_manifest(output_dir: Path) -> None:
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(MANIFEST, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"Wrote {manifest_path}")


def main() -> None:
    args = parse_args()
    output_dir = Path(args.output_dir)

    download(output_dir)
    write_manifest(output_dir)

    required = ["model.onnx", "tokenizer.json", "config.json", "manifest.json"]
    missing = [f for f in required if not (output_dir / f).exists()]
    if missing:
        print(f"ERROR: missing expected files after setup: {missing}", file=sys.stderr)
        sys.exit(1)

    print("\n" + "=" * 70)
    print("Model directory ready:", output_dir)
    print("=" * 70)
    print(f"""
Next steps:

1. Point lemond at a modernbert-capable ort-server build (the public 0.3.7
   release doesn't support modernbert yet). Add to lemond's config.json:

     {{"onnxruntime": {{"cpu_bin": "<path to your ort-server executable>"}}}}

   Restart lemond after editing config.json.

2. Register the model (checkpoint is the local directory above):

     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" -d "{{\\"model_name\\": \\"user.mmbert32k-pii-onnx\\", \\"checkpoint\\": \\"{output_dir.as_posix()}\\", \\"recipe\\": \\"onnxruntime\\"}}"

3. Register the router policy (uses the model registered in step 2 as a
   classifier - see test/conformance/routing/1/l2_pii_onnx_classifier/policy.json):

     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" --data-binary @test/conformance/routing/1/l2_pii_onnx_classifier/policy.json

4. Smoke test, then run the benchmark - reuses pii_routing_eval.py unmodified,
   same as every other router-based policy in this series:

     python test/eval/pii_routing_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron --policy ../l2_pii_onnx_classifier/policy.json --limit 20 --verbose

     python test/eval/pii_routing_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron --policy ../l2_pii_onnx_classifier/policy.json
""")


if __name__ == "__main__":
    main()
