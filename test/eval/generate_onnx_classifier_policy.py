"""
Generate test/conformance/routing/1/l2_pii_onnx_classifier/policy.json from
the mmbert32k-pii-detector ONNX model's label set.

The router's `classifier` match condition tests one label's score at a time,
and ort-server keeps all 35 raw BIO labels distinct rather than collapsing
B-/I- pairs (see setup_mmbert32k_onnx.py's docstring for why - the short
version: collapsing them would let one index's score silently overwrite the
other's instead of combining). That means "was any PII entity detected" has
to be spelled out as "does the B- or the I- variant of any of the 17 entity
types cross the threshold" - 34 leaf conditions - which is exactly the kind of
thing that invites a transcription error if hand-edited. Regenerate with this
script instead of hand-editing the policy's `any` block, e.g. after retuning
--min-score.

Usage:
    python test/eval/generate_onnx_classifier_policy.py [--min-score FLOAT] [--local-model NAME] [--cloud-model NAME] [--output FILE]

Defaults:
    --min-score    0.5
    --local-model  Qwen3.5-9B-NoThinking  (matches the baseline used by the
                   regex/LLM/embedding policies earlier in this series, for a
                   clean same-candidate comparison across mechanisms)
    --cloud-model  fireworks.kimi-k2p6
    --output       test/conformance/routing/1/l2_pii_onnx_classifier/policy.json
"""

import argparse
import json
from pathlib import Path

# Must match onnx/config.json's id2label on
# llm-semantic-router/mmbert32k-pii-detector-merged exactly (fetched and
# verified directly - see conversation). Duplicated from
# setup_mmbert32k_onnx.py's copy rather than imported, so this script has no
# import-path dependency on that one; keep the two in sync if the label set
# ever changes (e.g. a different model).
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

ONNX_MODEL_NAME = "user.mmbert32k-pii-onnx"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--min-score", type=float, default=0.5)
    p.add_argument("--local-model", default="Qwen3.5-9B-NoThinking")
    p.add_argument("--cloud-model", default="fireworks.kimi-k2p6")
    p.add_argument(
        "--output",
        default=str(
            Path(__file__).parent.parent
            / "conformance"
            / "routing"
            / "1"
            / "l2_pii_onnx_classifier"
            / "policy.json"
        ),
    )
    return p.parse_args()


def build_policy(min_score: float, local_model: str, cloud_model: str) -> dict:
    ordered = [v for _, v in sorted(ID2LABEL.items(), key=lambda kv: int(kv[0]))]
    non_o_labels = [label for label in ordered if label != "O"]

    return {
        "version": "1",
        "model_name": "user.PII-ONNX-Classifier-Router",
        "recipe": "collection.router",
        "components": [cloud_model, local_model, ONNX_MODEL_NAME],
        "routing": {
            "candidates": [local_model, cloud_model],
            "default_model": cloud_model,
            "classifiers": [
                {
                    "id": "pii-onnx",
                    "type": "classifier",
                    "model": ONNX_MODEL_NAME,
                    "labels": ordered,
                    "on_error": "match_false",
                }
            ],
            "rules": [
                {
                    "id": "pii-detected",
                    "match": {
                        "any": [
                            {
                                "classifier": "pii-onnx",
                                "label": label,
                                "min_score": min_score,
                            }
                            for label in non_o_labels
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
