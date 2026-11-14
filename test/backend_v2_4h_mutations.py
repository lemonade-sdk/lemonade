"""Compile production-registry and dependency regressions with named failing witnesses."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from llamacpp_oracle_audit import link_command

REPO = Path(__file__).resolve().parents[1]
ROOT = "src/cpp/server/backends/configured/"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--out", type=Path, default=REPO / ".synagent/artifacts/backend-v2-4h/mutations"
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4h/mutations",
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.scratch = args.scratch.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    cases = [
        (
            "production-contract-differs-qualified-literal",
            ROOT + "backend_definition_exports.cpp",
            "        const auto sources = make_v2_source_snapshot(embedded_v2_sources());\n        const auto audit =",
            "        ++impl->contracts[0].policy_revision;\n        impl->contracts[0].policy_digest = v2::export_policy_digest(impl->contracts[0]);\n        impl->digest = v2::export_registry_digest(impl->contracts, installed_hook_abis());\n        const auto sources = make_v2_source_snapshot(embedded_v2_sources());\n        const auto audit =",
            "test_backend_definition_exports",
            "ProductionContractEqualsQualifiedLiteral",
        ),
        (
            "production-source-pin-drift",
            ROOT + "backend_definition_exports.cpp",
            "        impl->contracts = {llamacpp_external_contract()};",
            "        impl->contracts = {llamacpp_external_contract()}; impl->contracts[0].expected_source_digest.back() ^= 1;",
            "test_llamacpp_external_fork",
            "Installed export registry audit failed",
        ),
        (
            "external-hook-authorship-allowed",
            ROOT + "backend_definition_authorization.cpp",
            'require(!hook_position(path), "authored_hook_selection", source);',
            'require(true, "authored_hook_selection", source);',
            "test_backend_definition_authorization",
            "DescendantCannotRegainRemovedHook",
        ),
        (
            "artifact-management-skips-required-producer",
            ROOT + "backend_definition_runner_internal.h",
            '                invoke<typed::PrepareHook>(frame, dependency, SlotOwnerKind::Root, "launch", "launch", {},\n                    [](auto target, auto&) { return target(typed::PrepareInput{}); });',
            "                (void)dependency;",
            "test_backend_runtime_services",
            "ExternalArtifactManagementInvokesBoundDependencies",
        ),
        (
            "artifact-management-admits-foreign-dependency",
            ROOT + "backend_definition_runner_internal.h",
            'if (slot.contract != typed::Contract::Prepare || slot.slot.owner_kind != SlotOwnerKind::Root ||\n                    slot.slot.owner_id != "launch" || slot.slot.stage != "launch")',
            "if (false)",
            "test_backend_runtime_services",
            "ArtifactManagementRejectsForeignDependency",
        ),
    ]
    cases.append(
        (
            "external-grandchild-bound-removed",
            "test/cpp/llamacpp_external_catalog_fixture.h",
            "limits.chain_documents = 2;",
            "limits.chain_documents = 3;",
            "test_backend_catalog_invalidation",
            "ExternalGraphBoundsFailClosed",
        )
    )
    results = []
    for name, source, old, new, target, assertion in cases:
        work = args.scratch / name
        work.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(work), TMP=str(work), TEMP=str(work))
        content = (REPO / source).read_text()
        assert content.count(old) == 1, (name, content.count(old))
        mutant = work / Path(source).name
        mutant.write_text(content.replace(old, new, 1))
        sources = (
            ["test/cpp/test_backend_catalog_invalidation.cpp"]
            if source.endswith("llamacpp_external_catalog_fixture.h")
            else (
                [source]
                if source.endswith(".cpp")
                else [
                    ROOT + "backend_definition_runner.cpp",
                    "test/cpp/test_backend_runtime_services.cpp",
                ]
            )
        )
        replacements = {}

        def execute(command, log, timeout=180):
            result = subprocess.run(
                command,
                cwd=args.build,
                env=env,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
            (work / log).write_text(result.stdout + result.stderr)
            return result

        for i, compiled_source in enumerate(sources):
            entries = [r for r in database if r["file"] == str(REPO / compiled_source)]
            entry = next(
                (r for r in entries if target + ".dir" in r["command"]), entries[0]
            )
            command = shlex.split(entry["command"])
            original = command[command.index("-o") + 1]
            obj = work / Path(original).name
            command[command.index("-o") + 1] = str(obj)
            if source.endswith(".cpp"):
                command[command.index("-c") + 1] = str(mutant)
            else:
                sibling = work / Path(compiled_source).name
                sibling.write_text((REPO / compiled_source).read_text())
                command[command.index("-c") + 1] = str(sibling)
                command.insert(1, "-I" + str(work))
            command = ["-O0" if a.startswith("-O") else a for a in command]
            command.extend(["-iquote", str((REPO / compiled_source).parent)])
            result = execute(command, f"compile-{i}.log")
            assert result.returncode == 0, (name, "compile", result.stderr[-3000:])
            replacements[original] = obj
        command = link_command(args.build, target)
        for original, obj in replacements.items():
            if original in command:
                command[command.index(original)] = str(obj)
            else:
                library = (
                    "liblemonade-backend-definition-v2.a"
                    if source.endswith(".cpp")
                    else "liblemonade-server-core.a"
                )
                assert library in command, (name, original)
                archive = work / library
                if not archive.exists():
                    shutil.copyfile(args.build / library, archive)
                assert (
                    execute(
                        ["ar", "r", str(archive), str(obj)], "archive.log"
                    ).returncode
                    == 0
                )
                command[command.index(library)] = str(archive)
        binary = work / "probe"
        command[command.index("-o") + 1] = str(binary)
        result = execute(command, "link.log")
        assert result.returncode == 0, (name, "link", result.stderr[-3000:])
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        observed = execute(["timeout", "90", str(binary)], "run.log", 100)
        assert observed.returncode == 1 and assertion in observed.stdout, (
            name,
            observed.returncode,
            observed.stdout[-6000:],
        )
        results.append(
            dict(
                rule=name,
                assertion=assertion,
                compiled=True,
                exit=observed.returncode,
                caught=True,
            )
        )
        (args.out / "compiled-mutations.json").write_text(
            json.dumps(results, indent=2) + "\n"
        )
        print(results[-1], flush=True)


if __name__ == "__main__":
    main()
