"""Require production Gufo data mutations to fail named CPU parity assertions."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]


def rules():
    def argv(document, identity, flag):
        next(
            row for row in document["merge"]["launch"]["argv"] if row["id"] == identity
        )["flag"] = flag

    def readiness(document):
        for field in ("readiness", "watchdog"):
            document["merge"]["launch"][field]["path"] = "/health"

    return (
        (
            "mount-layout",
            "ProductionGufoFixedExecutionOwner",
            lambda d: d["merge"]["launch"]["mounts"][0].update(layout="artifact_id"),
        ),
        (
            "device-set",
            "ProductionGufoFixedExecutionOwner",
            lambda d: d["merge"]["containers"]["rocm"].update(devices=["/dev/kfd"]),
        ),
        (
            "readiness-path",
            "ProductionGufoFrozenReadiness",
            readiness,
        ),
        (
            "argv-flag",
            "ProductionGufoFrozenArgv:dflash2",
            lambda d: argv(d, "context", "--ctx-size"),
        ),
        (
            "speculative-mapping",
            "ProductionGufoFrozenArgv:dflash2",
            lambda d: argv(d, "dflash2", "--mtp-model"),
        ),
        (
            "repository",
            "ProductionGufoFixedExecutionOwner",
            lambda d: d["merge"]["containers"]["rocm"].update(
                repository="docker.io/kyuz0/different"
            ),
        ),
        (
            "capability-overclaim",
            "ProductionGufoCapabilityMaskDoesNotOverclaim",
            lambda d: d["merge"]["metadata"].update(
                default_capabilities=["embeddings"]
            ),
        ),
        (
            "protocol-path",
            "ProductionGufoFrozenProtocolSurface",
            lambda d: d["merge"]["operations"][0]["transport"].update(
                path="/v1/embeddings"
            ),
        ),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str((args.scratch / "temporary").resolve()))
    environment.pop("LEMON_GUFO_PARITY_CANDIDATE", None)
    command = [
        "timeout",
        "60",
        str((args.build / "test_container_execution").resolve()),
    ]

    def run(name, env):
        result = subprocess.run(
            command, cwd=REPO, env=env, capture_output=True, text=True, timeout=65
        )
        (args.out / (name + ".log")).write_text(result.stdout + result.stderr)
        return result

    assert (
        run("control-before", environment).returncode == 0
    ), "GufoMutationControlMustPass"
    source = json.loads((REPO / "src/cpp/resources/backends/gufo.json").read_bytes())
    results = []
    for name, assertion, mutate in rules():
        document = copy.deepcopy(source)
        mutate(document)
        candidate = (args.scratch / (name + ".json")).resolve()
        candidate.write_text(json.dumps(document, indent=2) + "\n")
        result = run(
            name, dict(environment, LEMON_GUFO_PARITY_CANDIDATE=str(candidate))
        )
        assert result.returncode == 1 and "FAIL " + assertion in result.stdout, (
            name,
            result.returncode,
            result.stdout,
        )
        results.append(
            dict(
                rule=name, assertion=assertion, exit_code=result.returncode, caught=True
            )
        )
    assert (
        run("control-after", environment).returncode == 0
    ), "GufoMutationControlMustRemainGreen"
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(len(results), "production Gufo data mutations caught by named assertions")


if __name__ == "__main__":
    main()
