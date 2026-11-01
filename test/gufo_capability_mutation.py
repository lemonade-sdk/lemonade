"""Compile an overclaim in the real Gufo capability-seed projection."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("build", "out", "scratch"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    build, out, scratch = (
        getattr(args, name).resolve() for name in ("build", "out", "scratch")
    )
    for path in (out, scratch, scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch / "temporary"))

    def run(command, name, cwd=REPO):
        result = subprocess.run(
            command, cwd=cwd, env=env, capture_output=True, text=True, timeout=180
        )
        (out / (name + ".log")).write_text(result.stdout + result.stderr)
        return result

    suite = "test_container_execution"
    assert run(["timeout", "60", str(build / suite)], "control-before").returncode == 0
    header = (build / "include/gufo_parity_capabilities.h").read_text()
    assert header.count("{ nullptr, 0 }") == 1, "ExactProductionGufoCapabilitySeed"
    (scratch / "gufo_parity_capabilities.h").write_text(
        header.replace("{ nullptr, 0 }", "{ nullptr, lemon::CAP_EMBEDDINGS }")
    )
    database = json.loads((build / "compile_commands.json").read_bytes())
    source = REPO / "test/cpp/test_container_execution.cpp"
    row = next(row for row in database if Path(row["file"]) == source)
    command = shlex.split(row["command"])
    command.insert(1, "-I" + str(scratch))
    old_object = command[command.index("-o") + 1]
    obj, binary = scratch / "overclaim.o", scratch / suite
    command[command.index("-o") + 1] = str(obj)
    if "-MF" in command:
        command[command.index("-MF") + 1] = str(scratch / "overclaim.d")
    assert (
        run(command, "compile", Path(row["directory"])).returncode == 0
    ), "CompileFailureIsNotMutationKill"
    link = shlex.split(
        subprocess.check_output(
            ["ninja", "-t", "commands", suite], cwd=build, text=True
        )
        .splitlines()[-1]
        .removeprefix(": && ")
        .removesuffix(" && :")
    )
    assert old_object in link
    link = [
        str(obj) if item == old_object else item
        for item in link
        if not item.startswith("-Wl,--dependency-file=")
    ]
    link[link.index("-o") + 1] = str(binary)
    assert run(link, "link", build).returncode == 0, "LinkFailureIsNotMutationKill"
    resources = scratch / "resources"
    if not resources.exists():
        resources.symlink_to(build / "resources", target_is_directory=True)
    result = run(["timeout", "60", str(binary)], "mutation")
    assertion = "ProductionGufoCapabilityMaskDoesNotOverclaim"
    assert (
        result.returncode == 1 and "FAIL " + assertion in result.stdout
    ), "CapabilityOverclaimMustFailNamedAssertion"
    assert run(["timeout", "60", str(build / suite)], "control-after").returncode == 0
    (out / "results.json").write_text(
        json.dumps(
            [
                dict(
                    rule="capability-mask-overclaim",
                    old=0,
                    new="CAP_EMBEDDINGS",
                    assertion=assertion,
                    exit=1,
                    caught=True,
                )
            ],
            indent=2,
        )
        + "\n"
    )
    print("1/1 compiled Gufo capability-mask overclaim caught")


if __name__ == "__main__":
    main()
