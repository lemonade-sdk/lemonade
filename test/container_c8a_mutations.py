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


def checkpoint_rules():
    return [
        (
            "source-checkpoint-error",
            "backend_definition_runner.cpp",
            "throw std::runtime_error(evaluate(artifact.missing_error, frame).value().get<std::string>());",
            'throw std::runtime_error(artifact.records.contains("checkpoint_consumer") ? "mutated" : evaluate(artifact.missing_error, frame).value().get<std::string>());',
            "C8aCheckpointStillFailsWithoutExistingAbsolute",
        ),
        (
            "checkpoint-native-domain",
            "backend_execution_program.cpp",
            "require(program.model_contract == ModelContract::Ds4RocmV1 &&",
            "require(true &&",
            "C8aCheckpointRequiresContainerModelContract",
        ),
        (
            "available-resolved-fallback",
            "backend_definition_runner.cpp",
            'if (artifact.records.contains("checkpoint_consumer") && (path.empty() || !services.exists(path))) {',
            'if (artifact.records.contains("checkpoint_consumer")) {',
            "C8aAvailableResolvedAlwaysWins",
        ),
        (
            "relative-checkpoint",
            "backend_definition_runner.cpp",
            "!checkpoint.empty() && std::filesystem::path(checkpoint).is_absolute() && services.exists(checkpoint)",
            "!checkpoint.empty() && services.exists(checkpoint)",
            "C8aCheckpointStillFailsWithoutExistingAbsolute",
        ),
        (
            "fallback-inventory-bypass",
            "backend_definition_runner.cpp",
            "if (plan.container) materialize_container_mounts(program_, frame, plan,",
            'if (plan.container && !program_.normalized.at("artifacts").at("main").contains("checkpoint_consumer")) materialize_container_mounts(program_, frame, plan,',
            "C8aFallbackCannotBypassArtifactInventory",
        ),
        (
            "external-checkpoint-gate",
            "backend_definition_execution.cpp",
            'if (artifact.value().contains("checkpoint_consumer"))\n                    execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                        AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (artifact.value().contains("checkpoint_consumer"))\n                    execution::require(true,',
            "C8aExternalCheckpointCapabilityGate",
        ),
        (
            "checkpoint-consumer-unsealed",
            "backend_definition_authorization.cpp",
            'artifacts[name].erase("hook");',
            'artifacts[name].erase("hook"); artifacts[name].erase("checkpoint_consumer");',
            "C8aCheckpointConsumerSealed",
        ),
        (
            "checkpoint-optional-main",
            "backend_execution_program.cpp",
            'artifact.at("required_when") == true,',
            "true,",
            "C8aCheckpointClosedShape:required_when",
        ),
        (
            "checkpoint-no-inventory-mount",
            "backend_execution_program.cpp",
            'return mount.at("artifact") == name && mount.at("expand") == "file" && mount.value("when", Json(true)) == true;',
            'return mount.at("artifact") == name;',
            "C8aCheckpointRequiresInventoryMount",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--rule", action="append")
    parser.add_argument(
        "--contract", choices=("pre-install", "checkpoint"), default="pre-install"
    )
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
                suite + ".control", [args.build / suite, "--" + args.contract], REPO
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
    for name, filename, old, new, assertion, *header in (
        rules() if args.contract == "pre-install" else checkpoint_rules()
    ):
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
        result = run(name + ".run", [binary, "--" + args.contract], REPO)
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
