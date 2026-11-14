"""Mutation-check each C9 production definition with its unchanged passing control."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

from container_production_mutations import rules as production_rules

REPO = Path(__file__).resolve().parents[1]


def rules(document):
    result = production_rules(document)
    if document["recipe"] == "llamacpp":
        result.extend(
            [
                (
                    "pre-install-refusal-selection",
                    "C9FrozenPreInstallContract",
                    lambda d: d["merge"]["launch"].pop("pre_install_refusal"),
                ),
                (
                    "pre-install-refusal-order",
                    "C9FrozenPreInstallContract",
                    lambda d: d["merge"]["launch"].update(
                        prepare_order=[
                            "install",
                            "backend",
                            "hook",
                            "artifacts",
                            "ports",
                            "program",
                        ]
                    ),
                ),
            ]
        )
    else:
        result.append(
            (
                "memory-phase",
                "C9FrozenExecutionContract",
                lambda d: d["merge"]["launch"]["execution"]["cases"][0].pop(
                    "memory_phase"
                ),
            )
        )
        result.extend(
            [
                (
                    "checkpoint-consumer",
                    "C9FrozenArtifacts",
                    lambda d: d["merge"]["artifacts"]["main"].pop(
                        "checkpoint_consumer"
                    ),
                ),
                (
                    "checkpoint-fallback",
                    "C9FrozenArtifacts",
                    lambda d: d["merge"]["artifacts"]["main"].update(
                        absolute_checkpoint_fallback=False
                    ),
                ),
                (
                    "checkpoint-existence",
                    "C9FrozenArtifacts",
                    lambda d: d["merge"]["artifacts"]["main"].update(check="nonempty"),
                ),
            ]
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "out", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument(
        "--engine", choices=("halobox", "nathanw", "ds4_rocm"), required=True
    )
    args = parser.parse_args()
    for name in ("build", "out", "scratch"):
        setattr(args, name, getattr(args, name).resolve())
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )
    env.pop("LEMON_CONTAINER_PRODUCTION_CANDIDATE", None)

    def run(name, environment):
        result = subprocess.run(
            [
                "timeout",
                "60",
                str(args.build / "test_container_production"),
                args.engine,
            ],
            cwd=REPO,
            env=environment,
            capture_output=True,
            text=True,
            timeout=65,
        )
        (args.out / (name + ".log")).write_text(result.stdout + result.stderr)
        return result

    assert run("control-before", env).returncode == 0, "C9MutationControlMustPass"
    document = json.loads(
        (REPO / f"src/cpp/resources/backends/{args.engine}.json").read_bytes()
    )
    results = []
    for name, assertion, mutate in rules(document):
        candidate = copy.deepcopy(document)
        mutate(candidate)
        path = args.scratch / (name + ".json")
        path.write_text(json.dumps(candidate, indent=2) + "\n")
        result = run(name, dict(env, LEMON_CONTAINER_PRODUCTION_CANDIDATE=str(path)))
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
    assert run("control-after", env).returncode == 0, "C9MutationControlRemainsGreen"
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        args.engine,
        len(results),
        "data mutants caught with normal exit 1 and named assertions",
    )


if __name__ == "__main__":
    main()
