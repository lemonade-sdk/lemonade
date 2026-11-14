"""Require exact legacy equality and emit standalone identity comparisons."""

import argparse
import difflib
import json
from pathlib import Path


def canonical(value):
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def assert_v2_transition(before, after, output):
    left = before["cli_validation"]["v2"]["online"]
    right = after["cli_validation"]["v2"]["online"]
    if left == right:
        return
    catalog = before["schema"]["body"]["catalog_id"]
    old = {
        "catalog_id": catalog,
        "diagnostics": [
            {
                "code": "invalid_value",
                "definition": "snapshot-fixture",
                "message": "Incorrect constant",
                "pointer": "/schema_version",
            }
        ],
        "valid": False,
        "validation": "server",
    }
    new = {
        "catalog_id": catalog,
        "diagnostics": [
            {
                "code": "unknown_key",
                "definition": "snapshot-fixture",
                "message": "Definition admission failed",
                "pointer": "/kind",
            }
        ],
        "input_schema_version": 2,
        "stages": dict(
            resolved=False, authorized=False, catalog_admitted=False, executable=False
        ),
        "valid": False,
        "validation": "server",
    }
    assert left == dict(
        status=2, stderr="", stdout=canonical(old)
    ), "Unexpected historical online-v2 diagnostic"
    assert right == dict(
        status=2, stderr="", stdout=canonical(new)
    ), "Unexpected admitted-language online-v2 diagnostic"
    (output / "v2-online-before.json").write_text(canonical(left))
    (output / "v2-online-after.json").write_text(canonical(right))
    marker = {"separately_asserted": "flat v1 document with schema_version 2"}
    before["cli_validation"]["v2"]["online"] = marker
    after["cli_validation"]["v2"]["online"] = marker
    print("online-v2: exact old/new diagnostics asserted separately")


def compare(baseline, actual, output):
    before = json.loads(baseline.read_text())
    after = json.loads(actual.read_text())
    output.mkdir(parents=True, exist_ok=True)
    assert_v2_transition(before, after, output)
    selectors = {
        "legacy": lambda value: value,
        "schema": lambda value: value["schema"],
        "digests": lambda value: {
            "definitions": value["definitions"],
            "files": value["file_sha256"],
        },
        "etags": lambda value: {
            "full": value["schema"]["etag"],
            "filtered": {key: row["etag"] for key, row in value["filtered"].items()},
            "conditional": value["conditional"],
        },
    }
    equal = True
    for name, select in selectors.items():
        left, right = canonical(select(before)), canonical(select(after))
        (output / f"{name}-before.json").write_text(left)
        (output / f"{name}-after.json").write_text(right)
        difference = "".join(
            difflib.unified_diff(
                left.splitlines(True),
                right.splitlines(True),
                fromfile="baseline",
                tofile="actual",
            )
        )
        (output / f"{name}.diff").write_text(difference)
        equal &= not difference
        print(f"{name}: {len(difference.encode())} diff bytes")
    return equal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    raise SystemExit(0 if compare(args.baseline, args.actual, args.out) else 1)


if __name__ == "__main__":
    main()
