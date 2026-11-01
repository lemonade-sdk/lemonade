"""Require normal exit 1 and a named failing assertion for each C8a mutation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
SUITE = "test_container_production"


def rules():
    return [
        (
            "refusal-after-install",
            "backend_definition_runner.cpp",
            'if (phase == "backend" && plan.container && records.contains("pre_install_refusal")',
            'if (phase == "ports" && plan.container && records.contains("pre_install_refusal")',
            [
                "C8aRefusalBeforeAnyInstall:halobox",
                "C8aRefusalBeforeAnyInstall:nathanw",
            ],
        ),
        (
            "external-refusal",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].contains("pre_install_refusal"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].contains("pre_install_refusal"))\n                execution::require(true,',
            "C8aExternalRefusalRejected",
        ),
        (
            "refusal-unsealed",
            "backend_definition_authorization.cpp",
            'if (launch.contains("pre_install_refusal"))',
            "if (false)",
            ["C8aRefusalPolicySealed", "C8aRefusalOrderSealedWithoutDiagnostics"],
        ),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--rule", action="append")
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
    for suite in (SUITE,):
        assert (
            run(
                suite + ".control", [args.build / suite, "--pre-install"], REPO
            ).returncode
            == 0
        ), "C8aControlMustPass"
        commands = subprocess.check_output(
            ["ninja", "-t", "commands", suite], cwd=args.build, text=True, timeout=30
        ).splitlines()
        link = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
        links[suite] = [x for x in link if not x.startswith("-Wl,--dependency-file=")]
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    results = []
    for name, filename, old, new, assertion, *header in rules():
        if args.rule and name not in args.rule:
            continue
        suite = SUITE
        link = links[suite]
        assertions = assertion if isinstance(assertion, list) else [assertion]
        source = (
            REPO / filename if filename.startswith("test/") else CONFIGURED / filename
        )
        original = source.read_bytes()
        original_header = (REPO / header[0]).read_bytes() if header else None
        text = original_header.decode() if header else original.decode()
        assert text.count(old) == 1, (name, text.count(old))
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        mutant, obj, binary = work / source.name, work / "mutant.o", work / suite
        if header:
            mutated_header = work / Path(header[0]).name
            header_text = text.replace(old, new)
            if header[1] == "backend_diagnostics_internal.h":
                header_text = header_text.replace("inline ", "static inline ")
            mutated_header.write_text(header_text)
            mutant.write_text(
                original.decode().replace(
                    '#include "' + header[1] + '"',
                    '#include "' + str(mutated_header) + '"',
                )
            )
        else:
            mutant.write_text(text.replace(old, new))
        row = next(
            r
            for r in database
            if Path(r["file"]) == source
            and (
                "test_container_production.dir"
                if filename == "backend_definition_authorization.cpp"
                or filename.startswith("test/")
                else (
                    "lemonade-binding-test.dir"
                    if filename
                    in (
                        "backend_definition_execution.cpp",
                        "backend_execution_program.cpp",
                    )
                    else (
                        "lemonade-backend-definition-v2.dir"
                        if filename == "backend_definition_edits.cpp"
                        else "lemonade-server-core.dir"
                    )
                )
            )
            in r["command"]
        )
        compile_command = shlex.split(row["command"])
        old_object = compile_command[compile_command.index("-o") + 1]
        compile_command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in compile_command
        ]
        compile_command.insert(1, "-I" + str(CONFIGURED))
        compile_command.insert(1, "-I" + str(REPO / "test/cpp"))
        command = [str(obj) if x == old_object else x for x in link]
        if old_object not in link:
            command.insert(command.index("-o"), str(obj))
        command[command.index("-o") + 1] = str(binary)
        assert (
            run(name + ".compile", compile_command, Path(row["directory"])).returncode
            == 0
        ), (name + ": compile failure is not a kill")
        assert run(name + ".link", command, args.build).returncode == 0, (
            name + ": link failure is not a kill"
        )
        result = run(name + ".run", [binary, "--pre-install"], REPO)
        assert result.returncode == 1 and all(
            "FAIL " + label in (args.out / (name + ".run.log")).read_text()
            for label in assertions
        ), (name, result.returncode, assertion)
        assert (
            hashlib.sha256(source.read_bytes()).digest()
            == hashlib.sha256(original).digest()
        ), "ProductionSourceUntouched"
        if header:
            assert (
                REPO / header[0]
            ).read_bytes() == original_header, "ProductionHeaderUntouched"
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
    print(f"{len(results)}/{len(results)} C8a compiled mutations caught")


if __name__ == "__main__":
    main()
