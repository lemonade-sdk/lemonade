"""
Generate test/conformance/routing/1/l2_pii_onnx_pplx_masking/policy.json from
the pplx-pii-masking ONNX model's label set.

Same reasoning as generate_privacy_filter_onnx_policy.py: the router's
`classifier` match condition tests one label's score at a time, and this
model's manifest keeps all 37 raw BIOES labels distinct (9 PII types x
B/I/E/S, plus "O") rather than collapsing them, so "was any PII entity
detected" has to be spelled out as "does the B-, I-, E-, or S- variant of any
of the 9 PII types cross the threshold" - 36 leaf conditions - regenerate
with this script instead of hand-editing.

Usage:
    python test/eval/generate_pplx_pii_masking_onnx_policy.py [--min-score FLOAT] [--local-model NAME] [--cloud-model NAME] [--output FILE]

Defaults:
    --min-score    0.5
    --local-model  Qwen3.5-0.8B-GGUF
    --cloud-model  fireworks.kimi-k2p6
    --output       test/conformance/routing/1/l2_pii_onnx_pplx_masking/policy.json
"""

import argparse
import json
from pathlib import Path

# Must match modeling_pii_masking.py's PII_TYPES exactly (fetched and
# verified directly from the repo - see conversation). Duplicated from
# setup_pplx_pii_masking_onnx.py's copy rather than imported, so this script
# has no import-path dependency on that one; keep the two in sync if the
# label set ever changes.
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
NON_O_LABELS = [f"{tag}-{t}" for t in PII_TYPES for tag in "BIES"]
assert len(NON_O_LABELS) == 36

ONNX_MODEL_NAME = "user.pplx-pii-masking-onnx"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--min-score", type=float, default=0.5)
    p.add_argument("--local-model", default="Qwen3.5-0.8B-GGUF")
    p.add_argument("--cloud-model", default="fireworks.kimi-k2p6")
    p.add_argument(
        "--output",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_onnx_pplx_masking"
            / "policy.json"
        ),
    )
    return p.parse_args()


def build_policy(min_score: float, local_model: str, cloud_model: str) -> dict:
    return {
        "version": "1",
        "model_name": "user.PII-ONNX-PplxMasking-Router",
        "recipe": "collection.router",
        "components": [cloud_model, local_model, ONNX_MODEL_NAME],
        "routing": {
            "candidates": [local_model, cloud_model],
            "default_model": cloud_model,
            "classifiers": [
                {
                    "id": "pii-onnx-pplx",
                    "type": "classifier",
                    "model": ONNX_MODEL_NAME,
                    "labels": ["O"] + NON_O_LABELS,
                    "on_error": "match_false",
                }
            ],
            "rules": [
                {
                    "id": "pii-detected",
                    "match": {
                        "any": [
                            {
                                "classifier": "pii-onnx-pplx",
                                "label": label,
                                "min_score": min_score,
                            }
                            for label in NON_O_LABELS
                        ]
                    },
                    "route_to": local_model,
                }
            ],
        },
    }


def main() -> None:
    args = parse_args()
    policy = build_policy(args.min_score, args.local_model, args.cloud_model)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(policy, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    n_leaves = len(policy["routing"]["rules"][0]["match"]["any"])
    print(f"Wrote {output_path} ({n_leaves} match leaves, min_score={args.min_score})")


if __name__ == "__main__":
    main()
