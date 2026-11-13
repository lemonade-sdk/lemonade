"""Preserve protected production authority and independently qualified export sources."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[1]
BASE = "476c66303ab21990469a8226c8287a091ec80de7"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out", type=Path, default=REPO / ".synagent/artifacts/backend-v2-4h"
    )
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    paths = [
        "schemas/backend-definition-v1.schema.json",
        "schemas/backend-definition-v2.schema.json",
        "src/cpp/server/backends/configured/backend_hook_abi.cpp",
        "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
        "src/cpp/server/backends/configured/backend_definition_runner.cpp",
        "test/cpp/llamacpp_external_contract.h",
        "test/fixtures/backend-v2/hook-abi.inventory.json",
        "test/fixtures/backend-v2/llamacpp.catalog-transition.json",
        "test/fixtures/backend-v2/hrx.catalog-transition.json",
        "test/fixtures/backend-v2/llamacpp.production-evidence.json",
        "test/fixtures/backend-v2/external/llamacpp.pins.json",
        "test/fixtures/backend-v2/external/research-llama.json",
        "test/fixtures/backend-v2/external/fork-b.json",
    ]
    paths += [
        str(p.relative_to(REPO))
        for p in sorted((REPO / "src/cpp/resources/backends").glob("*.json"))
    ]
    records = []
    for name in paths:
        before = subprocess.check_output(
            ["git", "show", f"{BASE}:{name}"], cwd=REPO, timeout=30
        )
        actual = (REPO / name).read_bytes()
        assert before == actual, "ProtectedSource:" + name
        records.append(dict(path=name, sha256=hashlib.sha256(actual).hexdigest()))
    source = (
        REPO / "src/cpp/server/backends/configured/backend_llamacpp_export.cpp"
    ).read_bytes()
    literals = re.findall(rb'R"([A-Za-z0-9_]*)\((.*?)\)\1"', source, re.DOTALL)
    assert literals and all(
        len(value) <= 8000 for _, value in literals
    ), "ProductionExportLiteralBudget"
    seven = {
        "ds4",
        "kokoro",
        "moonshine",
        "onnxruntime",
        "ryzenai",
        "thinksound",
        "trellis",
    }
    inventory = {
        p.stem: json.loads(p.read_bytes())["schema_version"]
        for p in (REPO / "src/cpp/resources/backends").glob("*.json")
    }
    assert {n for n, v in inventory.items() if v == 1} == seven and {
        n for n, v in inventory.items() if v == 2
    } == {"hrx", "llamacpp"}, "SevenV1TwoV2Exact"
    (args.out / "hard-unchanged.json").write_text(
        json.dumps(
            dict(
                base=BASE,
                unchanged=records,
                production_contract_literals=len(literals),
                maximum_literal=max(len(value) for _, value in literals),
            ),
            indent=2,
        )
        + "\n"
    )
    print(
        len(records),
        "protected source files unchanged; seven v1, two v2, nineteen ABIs, two builtin policies, both historical transitions and independent export literal/pins retained",
    )


if __name__ == "__main__":
    main()
