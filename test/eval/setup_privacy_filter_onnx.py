"""
Place the corrected privacy-filter-multilingual-v2 ONNX export (produced by
test/eval/privacy_filter2onnx.py) alongside the model's HF-cache snapshot so
Lemonade's onnxruntime backend can serve it, and write the manifest.json that
declares its output contract.

Same registration pattern as test/eval/setup_mmbert32k_onnx.py: /v1/pull's
`checkpoint` field is always validated as an HF repo id (local filesystem
paths are rejected regardless of recipe - confirmed empirically), so the
model gets registered with checkpoint="OpenMed/privacy-filter-multilingual-v2"
(downloading the safetensors + tokenizer/config, which this script does NOT
need - it's already cached from the earlier standalone benchmark). ort-server
then needs model.onnx (+ its external-data companion) sitting next to that
resolved snapshot's config.json/tokenizer.json, which is what this script
copies (hardlinked when the cache and export live on the same volume, so the
5.6GB weights aren't duplicated on disk).

A manifest is required: this is a token-classification model (BIOES-tagged
over 54 entity types, 217 labels including "O"), and ort-server's manifest-less
inference defaults to single-label text-classification.

Requirements:
    pip install huggingface_hub

Usage:
    python test/eval/setup_privacy_filter_onnx.py [--onnx-dir DIR] [--min-score FLOAT] [--base-url URL]
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

HF_REPO = "OpenMed/privacy-filter-multilingual-v2"
LEMONADE_MODEL_NAME = "user.privacy-filter-ml-v2-onnx"

# From config.json's id2label, fetched and verified directly (see
# conversation) - kept as a literal here rather than re-derived at manifest-
# write time so this script has no dependency on the ONNX export or a live
# HF fetch succeeding in a specific order.
ID2LABEL = {
    "0": "O",
}
_ENTITY_TYPES = [
    "ACCOUNTNAME",
    "AGE",
    "AMOUNT",
    "BANKACCOUNT",
    "BIC",
    "BITCOINADDRESS",
    "BUILDINGNUMBER",
    "CITY",
    "COUNTY",
    "CREDITCARD",
    "CREDITCARDISSUER",
    "CURRENCY",
    "CURRENCYCODE",
    "CURRENCYNAME",
    "CURRENCYSYMBOL",
    "CVV",
    "DATE",
    "DATEOFBIRTH",
    "EMAIL",
    "ETHEREUMADDRESS",
    "EYECOLOR",
    "FIRSTNAME",
    "GENDER",
    "GPSCOORDINATES",
    "HEIGHT",
    "IBAN",
    "IMEI",
    "IPADDRESS",
    "JOBDEPARTMENT",
    "JOBTITLE",
    "LASTNAME",
    "LITECOINADDRESS",
    "MACADDRESS",
    "MASKEDNUMBER",
    "MIDDLENAME",
    "OCCUPATION",
    "ORDINALDIRECTION",
    "ORGANIZATION",
    "PASSWORD",
    "PHONE",
    "PIN",
    "PREFIX",
    "SECONDARYADDRESS",
    "SEX",
    "SSN",
    "STATE",
    "STREET",
    "TIME",
    "URL",
    "USERAGENT",
    "USERNAME",
    "VIN",
    "VRM",
    "ZIPCODE",
]
_idx = 1
for _entity in _ENTITY_TYPES:
    for _tag in ("B", "I", "E", "S"):
        ID2LABEL[str(_idx)] = f"{_tag}-{_entity}"
        _idx += 1
assert len(ID2LABEL) == 217, f"expected 217 labels, got {len(ID2LABEL)}"

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
            Path.home()
            / ".cache"
            / "lemonade-onnx-models"
            / "privacy-filter-ml-v2-onnx"
        ),
        help="Directory containing model.onnx + model.onnx.data, produced by "
        "test/eval/privacy_filter2onnx.py",
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

    # config.json is tiny and already cached from privacy_filter2onnx.py's
    # own download - this just resolves the local snapshot path without
    # re-fetching anything.
    config_path = Path(hf_hub_download(repo_id=HF_REPO, filename="config.json"))
    return config_path.parent


def place_onnx_files(onnx_dir: Path, snapshot_dir: Path) -> None:
    for filename in ("model.onnx", "model.onnx.data"):
        src = onnx_dir / filename
        if not src.exists():
            print(
                f"ERROR: {src} not found - run privacy_filter2onnx.py first",
                file=sys.stderr,
            )
            sys.exit(1)
        dst = snapshot_dir / filename
        if dst.exists():
            dst.unlink()
        try:
            import os

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

    required = [
        "model.onnx",
        "model.onnx.data",
        "tokenizer.json",
        "config.json",
        "manifest.json",
    ]
    missing = [f for f in required if not (snapshot_dir / f).exists()]
    if missing:
        print(f"ERROR: missing expected files after setup: {missing}", file=sys.stderr)
        sys.exit(1)

    print("\n" + "=" * 70)
    print("Model directory ready:", snapshot_dir)
    print("=" * 70)
    print(f"""
Next steps:

1. lemond must be pointed at the modernbert/openai_privacy_filter-capable
   ort-server build (config.json: {{"onnxruntime": {{"cpu_bin": "<path>"}}}}),
   same as for mmbert32k-pii-onnx.

2. Register the model (checkpoint is the real HF repo id - downloads the
   safetensors/tokenizer/config, already cached, alongside the ONNX files
   just placed above):

     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" -d "{{\\"model_name\\": \\"{LEMONADE_MODEL_NAME}\\", \\"checkpoint\\": \\"{HF_REPO}\\", \\"recipe\\": \\"onnxruntime\\"}}"

3. Generate + register the router policy:

     python test/eval/generate_privacy_filter_onnx_policy.py --min-score {args.min_score}
     curl -X POST {args.base_url}/v1/pull -H "Content-Type: application/json" --data-binary @test/conformance/routing/1/l2_pii_onnx_privacy_filter/policy.json

4. Smoke test, then run the benchmark:

     python test/eval/pii_routing_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --policy ../l2_pii_onnx_privacy_filter/policy.json --limit 20 --verbose
""")


if __name__ == "__main__":
    main()
