"""Require normal exit 1 and a named failing assertion for each C6 mutation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
SUITE = "test_backend_definition_runner"


def rules():
    return [
        (
            "raw-label-suppressed",
            "backend_definition_runner.cpp",
            '{"mtp", has("mtp")}',
            '{"mtp", false}',
            "RawMtpStackArgv:dflash-mtp",
        ),
        (
            "raw-label-conflated",
            "backend_definition_runner.cpp",
            '{"mtp", has("mtp")}',
            '{"mtp", has("mtp") && !has("dflash")}',
            "RawMtpStackArgv:dflash-mtp",
        ),
        (
            "raw-dflash-suppressed",
            "backend_definition_runner.cpp",
            '{"dflash", has("dflash")}',
            '{"dflash", false}',
            "RawLabelFacts:dflash",
        ),
        (
            "device-validation-api-ignored",
            "backend_definition_runner.cpp",
            'const auto validation_backend = validation_api == execution::DeviceValidationBackend::Rocm ? "rocm" :\n                    validation_api == execution::DeviceValidationBackend::Vulkan ? "vulkan" : backend;',
            "const auto validation_backend = backend;",
            ["MappedDeviceAccepted:halobox", "MappedDeviceAccepted:nathanw"],
            "test_container_execution",
        ),
        (
            "device-validation-rejection-ignored",
            "backend_definition_runner.cpp",
            "if (!checked.supported) throw std::invalid_argument(checked.error);",
            "if (false) throw std::invalid_argument(checked.error);",
            ["MappedWrongDeviceRejected:halobox", "MappedWrongDeviceRejected:nathanw"],
            "test_container_execution",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    args.build, args.out, args.scratch = (
        p.resolve() for p in (args.build, args.out, args.scratch)
    )
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
    )

    def run(name, command, cwd):
        with (args.out / (name + ".log")).open("w") as log:
            return subprocess.run(
                ["timeout", "180", *map(str, command)],
                cwd=cwd,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=185,
            )

    links = {}
    for suite in (SUITE, "test_container_execution"):
        assert (
            run(suite + ".control", [args.build / suite], REPO).returncode == 0
        ), "C6ControlMustPass"
        commands = subprocess.check_output(
            ["ninja", "-t", "commands", suite], cwd=args.build, text=True, timeout=30
        ).splitlines()
        link = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
        links[suite] = [x for x in link if not x.startswith("-Wl,--dependency-file=")]
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    results = []
    for name, filename, old, new, assertion, *target in rules():
        suite = target[0] if target else SUITE
        link = links[suite]
        assertions = assertion if isinstance(assertion, list) else [assertion]
        source = CONFIGURED / filename
        original = source.read_bytes()
        text = original.decode()
        assert text.count(old) == 1, (name, text.count(old))
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        mutant, obj, binary = work / filename, work / "mutant.o", work / suite
        mutant.write_text(text.replace(old, new))
        row = next(
            r
            for r in database
            if Path(r["file"]) == source and "lemonade-server-core.dir" in r["command"]
        )
        compile_command = shlex.split(row["command"])
        old_object = compile_command[compile_command.index("-o") + 1]
        compile_command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in compile_command
        ]
        compile_command.insert(1, "-I" + str(CONFIGURED))
        assert (
            run(name + ".compile", compile_command, Path(row["directory"])).returncode
            == 0
        ), (name + ": compile failure is not a kill")
        command = [str(obj) if x == old_object else x for x in link]
        if old_object not in link:
            command.insert(command.index("-o"), str(obj))
        command[command.index("-o") + 1] = str(binary)
        assert run(name + ".link", command, args.build).returncode == 0, (
            name + ": link failure is not a kill"
        )
        result = run(name + ".run", [binary], REPO)
        assert result.returncode == 1 and all(
            "FAIL " + label in (args.out / (name + ".run.log")).read_text()
            for label in assertions
        ), (name, result.returncode, assertion)
        assert (
            hashlib.sha256(source.read_bytes()).digest()
            == hashlib.sha256(original).digest()
        ), "ProductionSourceUntouched"
        results.append(
            dict(
                rule=name,
                source=str(source.relative_to(REPO)),
                assertion=assertion,
                exit=result.returncode,
                caught=True,
            )
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name + ": caught by " + ", ".join(assertions), flush=True)
    print(f"{len(results)}/{len(results)} C6 compiled mutations caught")


if __name__ == "__main__":
    main()
