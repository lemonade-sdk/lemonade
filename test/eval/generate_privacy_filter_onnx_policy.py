"""
Generate test/conformance/routing/1/l2_pii_onnx_privacy_filter/policy.json from
the privacy-filter-multilingual-v2 ONNX model's label set.

Same reasoning as generate_onnx_classifier_policy.py: the router's
`classifier` match condition tests one label's score at a time, and this
model's manifest keeps all 217 raw BIOES labels distinct (54 entity types x
B/I/E/S, plus "O") rather than collapsing them - the token_aggregation="max"
scheme's per-label scores land in a std::map<string,float> via plain
assignment (see ort-server's main.cpp), so collapsing labels would let one
tag's score silently overwrite another's instead of combining them. That
means "was any PII entity detected" has to be spelled out as "does the B-,
I-, E-, or S- variant of any of the 54 entity types cross the threshold" -
216 leaf conditions - regenerate with this script instead of hand-editing.

Usage:
    python test/eval/generate_privacy_filter_onnx_policy.py [--min-score FLOAT] [--local-model NAME] [--cloud-model NAME] [--output FILE]

Defaults:
    --min-score    0.5
    --local-model  Qwen3.5-0.8B-GGUF
    --cloud-model  fireworks.kimi-k2p6
    --output       test/conformance/routing/1/l2_pii_onnx_privacy_filter/policy.json
"""

import argparse
import json
from pathlib import Path

# Must match the checkpoint's config.json id2label exactly (54 entity types x
# BIOES, verified directly against the live config - see conversation).
# Duplicated from setup_privacy_filter_onnx.py's copy rather than imported, so
# this script has no import-path dependency on that one.
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
NON_O_LABELS = [
    f"{tag}-{entity}" for entity in _ENTITY_TYPES for tag in ("B", "I", "E", "S")
]
assert len(NON_O_LABELS) == 216

ONNX_MODEL_NAME = "user.privacy-filter-ml-v2-onnx"


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
            / "l2_pii_onnx_privacy_filter"
            / "policy.json"
        ),
    )
    return p.parse_args()


def build_policy(min_score: float, local_model: str, cloud_model: str) -> dict:
    return {
        "version": "1",
        "model_name": "user.PII-ONNX-PrivacyFilter-Router",
        "recipe": "collection.router",
        "components": [cloud_model, local_model, ONNX_MODEL_NAME],
        "routing": {
            "candidates": [local_model, cloud_model],
            "default_model": cloud_model,
            "classifiers": [
                {
                    "id": "pii-onnx-pf",
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
                                "classifier": "pii-onnx-pf",
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
