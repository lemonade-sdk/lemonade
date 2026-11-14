"""Fail-closed sidecar mutations; the gate always executes the real candidate."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def mutations(value):
    invalid = copy.deepcopy(value)
    invalid["schema_version"] = 2
    yield "schema-version", invalid, "SidecarEnvelopeExact"
    invalid = copy.deepcopy(value)
    invalid["extra"] = True
    yield "extra-envelope-field", invalid, "SidecarEnvelopeExact"
    invalid = copy.deepcopy(value)
    invalid["fixtures"].pop("pins.json")
    yield "missing-fixture-hash", invalid, "FrozenFixtureHashSetExact"
    invalid = copy.deepcopy(value)
    invalid["families"][0]["id"] = "other-family"
    yield "foreign-family", invalid, "ExactlyOneBoundedProbeFamily"
    invalid = copy.deepcopy(value)
    invalid["families"][0]["records"][0]["extra"] = True
    yield "extra-record-field", invalid, "ProbeRecordFieldsExact"
    invalid = copy.deepcopy(value)
    invalid["families"][0]["records"][0]["view"] = "other-view"
    yield "foreign-view", invalid, "OnlyTenVersionCasesAcrossFourViews"
    invalid = copy.deepcopy(value)
    invalid["families"][0]["records"][0]["new"] = ["changed"]
    yield "foreign-new-value", invalid, "ProbeNewValueExactAndNecessary"
    missing = copy.deepcopy(value)
    missing["families"][0]["records"].pop()
    yield "missing-record", missing, "Exactly40ProbeRecords"
    extra = copy.deepcopy(value)
    extra["families"].append(copy.deepcopy(extra["families"][0]))
    yield "extra-family", extra, "ExactlyOneBoundedProbeFamily"
    extra = copy.deepcopy(value)
    extra["families"][0]["records"].append(
        copy.deepcopy(extra["families"][0]["records"][0])
    )
    yield "extra-record", extra, "Exactly40ProbeRecords"
    stale = copy.deepcopy(value)
    stale["fixtures"]["linux_x64.json"] = "sha256:" + "0" * 64
    yield "stale-hash", stale, "FrozenFixtureHashExact:linux_x64"
    unused = copy.deepcopy(value)
    unused["families"][0]["records"][0]["new"] = unused["families"][0]["records"][0][
        "old"
    ]
    yield "unused-record", unused, "ProbeNewValueExactAndNecessary"
    foreign = copy.deepcopy(value)
    foreign["families"][0]["records"][0]["case"] = "hip-415"
    yield "foreign-case", foreign, "OnlyTenVersionCasesAcrossFourViews"
    stale = copy.deepcopy(value)
    stale["families"][0]["records"][0]["old"] = []
    yield "stale-old-value", stale, "ProbeOldValueExact"
    duplicate = copy.deepcopy(value)
    duplicate["families"][0]["records"][1] = copy.deepcopy(
        duplicate["families"][0]["records"][0]
    )
    yield "duplicate-record", duplicate, "NoDuplicateProbeRecords"
    value = copy.deepcopy(value)
    value["families"][0]["records"][0]["pointer"] = "/system"
    yield "foreign-pointer", value, "OnlyTenVersionCasesAcrossFourViews"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    for directory in (args.scratch, args.out):
        directory.resolve().relative_to(ROOT)
        directory.mkdir(parents=True, exist_ok=True)
    temp = args.scratch.resolve() / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    binary = args.build.resolve() / "test_llamacpp_v2_deliberate_diff"
    subprocess.run(["timeout", "120s", str(binary)], env=env, check=True)
    value = json.loads(
        (ROOT / "test/fixtures/backend-v2/llamacpp.deliberate-diffs.json").read_text()
    )
    results = []
    for name, changed, assertion in mutations(value):
        path = args.scratch.resolve() / (name + ".json")
        path.write_text(json.dumps(changed, indent=2) + "\n")
        result = subprocess.run(
            ["timeout", "120s", str(binary), "--sidecar", str(path)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        output = result.stdout + result.stderr
        (args.out / (name + ".log")).write_text(output)
        caught = result.returncode == 1 and "FAIL " + assertion in output
        results.append(
            dict(
                rule=name,
                assertion=assertion,
                returncode=result.returncode,
                caught=caught,
            )
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        assert caught, (name, output)
    print(f"{len(results)} sidecar mutations caught")


if __name__ == "__main__":
    main()
