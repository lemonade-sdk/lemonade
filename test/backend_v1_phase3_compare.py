"""Require exact legacy equality and emit standalone identity comparisons."""

import argparse
import difflib
import json
from pathlib import Path


def canonical(value):
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def compare(baseline, actual, output):
    before = json.loads(baseline.read_text())
    after = json.loads(actual.read_text())
    output.mkdir(parents=True, exist_ok=True)
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
