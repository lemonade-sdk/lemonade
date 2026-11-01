"""Kill each production data mutant with a named assertion and normal exit 1."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]


def rules(document):
    def argv(d):
        rows = d["merge"]["launch"]["argv"]
        if rows:
            rows[0]["flag"] = "--wrong-model"
        else:
            rows.append(dict(id="unrequested", args=["--wrong-flag"]))

    def mount(d):
        d["merge"]["launch"]["mounts"][0]["destination"] = "/wrong"

    def devices(d):
        next(iter(d["merge"]["containers"].values()))["devices"] = []

    def repository(d):
        next(iter(d["merge"]["containers"].values()))[
            "repository"
        ] = "docker.io/kyuz0/foreign"

    def pin(d):
        next(iter(d["merge"]["containers"].values()))["pin"]["backend"] = "foreign"

    def diagnostic(d):
        d["merge"]["launch"]["diagnostics"][0]["level"] = "error"

    result = [
        ("argv-flag", "C8Frozenargv", argv),
        ("mount-destination", "C8Frozenmounts", mount),
        ("device-set", "C8FrozenContainerPolicy", devices),
        (
            "readiness-path",
            "C8Frozenreadiness",
            lambda d: d["merge"]["launch"]["readiness"].update(path="/wrong"),
        ),
        ("repository", "C8FrozenContainerPolicy", repository),
        ("pin-source", "C8FrozenContainerPolicy", pin),
        ("diagnostic-line", "C8Frozendiagnostics", diagnostic),
        (
            "capability-mask",
            "C8FrozenCapabilityAndMetadata",
            lambda d: d["merge"]["metadata"].update(
                default_capabilities=["embeddings"]
            ),
        ),
    ]
    if len(document["merge"]["launch"]["diagnostics"]) > 1:
        result.append(
            (
                "diagnostic-order",
                "C8Frozendiagnostics",
                lambda d: d["merge"]["launch"]["diagnostics"].reverse(),
            )
        )
    if document["id"] == "ds4_rocm":
        result.append(
            (
                "rewrite-domain",
                "C8FrozenRewriteAndProtocols",
                lambda d: d["merge"]["operations"][0]["request"].update(
                    rewrite_domain="canonical_path"
                ),
            )
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str((args.scratch / "temporary").resolve()))
    environment.pop("LEMON_CONTAINER_PRODUCTION_CANDIDATE", None)
    command = [
        "timeout",
        "60",
        str((args.build / "test_container_production").resolve()),
        args.engine,
    ]

    def run(name, env):
        result = subprocess.run(
            command, cwd=REPO, env=env, capture_output=True, text=True, timeout=65
        )
        (args.out / (name + ".log")).write_text(result.stdout + result.stderr)
        return result

    assert (
        run("control-before", environment).returncode == 0
    ), "C8MutationControlMustPass"
    source = json.loads(
        (REPO / f"src/cpp/resources/backends/{args.engine}.json").read_bytes()
    )
    results = []
    for name, assertion, mutate in rules(source):
        changed = copy.deepcopy(source)
        mutate(changed)
        candidate = (args.scratch / (name + ".json")).resolve()
        candidate.write_text(json.dumps(changed, indent=2) + "\n")
        result = run(
            name, dict(environment, LEMON_CONTAINER_PRODUCTION_CANDIDATE=str(candidate))
        )
        label = assertion + ":" + args.engine
        assert result.returncode == 1 and "FAIL " + label in result.stdout, (
            name,
            result.returncode,
            result.stdout,
        )
        results.append(
            dict(
                engine=args.engine, rule=name, assertion=label, exit_code=1, caught=True
            )
        )
    assert (
        run("control-after", environment).returncode == 0
    ), "C8MutationControlRemainsGreen"
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        args.engine,
        len(results),
        "data mutants caught by normal exit 1 and named assertion",
    )


if __name__ == "__main__":
    main()
