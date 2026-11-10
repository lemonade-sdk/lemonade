"""Compile isolated external-contract regressions and require named failures."""

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
        "--out", type=Path, default=REPO / ".synagent/artifacts/backend-v2-4g"
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4g/mutations",
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.scratch = args.scratch.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    cases = [
        (
            "external-builtin-staging-privilege",
            ROOT + "backend_definition_runner.cpp",
            "executable_->compiled().authorized().definition().key.space == DefinitionNamespace::Builtin))",
            "true))",
            "test_backend_runtime_services",
            "RealExternalLeafNeverGetsBuiltinExternalStagingAuthority",
        ),
        (
            "removed-owner-returns",
            ROOT + "backend_definition_authorization.cpp",
            'require(owner(parent.declaration, slot, budget) != nullptr, "owner_reintroduced", source);',
            'require(true, "owner_reintroduced", source);',
            "test_backend_definition_owner_graph",
            "FinalImmediateParentOwnerSubset",
        ),
        (
            "test-injection-leaks-installed-singleton",
            ROOT + "backend_definition_exports.cpp",
            "const BuiltinExportRegistry& installed_builtin_exports() { return BuiltinCatalogLoader::installed_exports(); }",
            "const BuiltinExportRegistry& installed_builtin_exports() { return BuiltinCatalogLoader::selected_exports(); }",
            "test_llamacpp_external_fork",
            "ProductionExportsRemainEmpty",
        ),
        (
            "eleven-slot-contract-accepted",
            ROOT + "backend_definition_exports.cpp",
            'if (selected != policies) error(id, "uncovered_hook_slot", "");',
            'if (false) error(id, "uncovered_hook_slot", "");',
            "test_backend_definition_exports",
            "ExternalContractAuditRejects:uncovered_hook_slot",
        ),
        (
            "mutable-options-widened",
            ROOT + "backend_definition_policy.cpp",
            'for (const auto& name : local_install_fields()) declaration.at("install").erase(name);',
            'declaration.at("metadata").erase("options"); for (const auto& name : local_install_fields()) declaration.at("install").erase(name);',
            "test_backend_definition_mutable_policy",
            "UnsetSentinelsCannotChange",
        ),
        (
            "partial-program-transition",
            ROOT + "backend_definition_policy.cpp",
            'doc.at("launch").at("program") == Json{{"source", "external_or_managed"}, {"install_policy", "always"}}',
            "true",
            "test_backend_definition_mutable_policy",
            "LocalExecutableProfileIsClosed:/launch/program/source",
        ),
        (
            "local-install-profile-opened",
            ROOT + "backend_definition_policy.cpp",
            'doc.at("install") == Json{{"strategy", "external_only"}, {"install_recipe", recipe},\n            {"pin_recipe", recipe}, {"cases", Json::array()}}',
            "true",
            "test_backend_definition_mutable_policy",
            "LocalExecutableProfileIsClosed:/install/install_recipe",
        ),
        (
            "fixture-uses-builtin-executable",
            "test/cpp/test_llamacpp_external_fork.cpp",
            "auto effects = production_backend_effects(nullptr, [=] { return port; });",
            "auto effects = production_backend_effects(nullptr, [=] { return port; }); effects.binary.find_external = [](const auto&, const auto&) { return std::string(LEMON_FAKE_ENGINE); };",
            "test_llamacpp_external_fork",
            "ExternalForkNeverUsesBuiltinExecutable",
        ),
    ]
    cases.extend(
        [
            (
                "binary-basename-rule-removed",
                ROOT + "backend_definition_policy.cpp",
                'require_policy(identifier(name) && name != "." && name != "..", "invalid_local_executable_profile");',
                'require_policy(true, "invalid_local_executable_profile");',
                "test_backend_definition_mutable_policy",
                "LocalExecutableBinaryIsBasename",
            ),
            (
                "external-cli-flags-allowed",
                ROOT + "backend_definition_policy.cpp",
                'require_policy(!option.contains("cli_flag"), "external_named_flag");',
                'require_policy(true, "external_named_flag");',
                "test_backend_definition_mutable_policy",
                "NoExternalNamedFlags",
            ),
            (
                "external-port-order-unavailable",
                ROOT + "backend_definition_provenance.cpp",
                '(doc["install"]["strategy"] != "external_only" && position("program") >= position("ports"))',
                'position("program") >= position("ports")',
                "test_llamacpp_external_fork",
                "ExternalForkDeclarationToProcessExact",
            ),
        ]
    )
    cases.append(
        (
            "trusted-audit-form-used-by-external-leaf",
            ROOT + "backend_definition_policy.cpp",
            "if (auditing) {",
            "if (doc == base) {",
            "test_backend_definition_mutable_policy",
            "TrustedBaseAuditCannotAuthorizeExternalIdentity",
        )
    )
    results = []
    for name, source, old, new, target, assertion in cases:
        work = args.scratch / name
        work.mkdir(parents=True, exist_ok=True)
        temporary = work / "temp"
        temporary.mkdir(exist_ok=True)
        env = dict(
            os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary)
        )
        text = (REPO / source).read_text()
        assert text.count(old) == 1, (name, text.count(old))
        mutant = work / Path(source).name
        mutant.write_text(text.replace(old, new, 1))
        entries = [row for row in database if row["file"] == str(REPO / source)]
        entry = next(
            (row for row in entries if target + ".dir" in row["command"]), entries[0]
        )
        command = shlex.split(entry["command"])
        original = command[command.index("-o") + 1]
        obj = work / Path(original).name
        command[command.index("-o") + 1] = str(obj)
        command[command.index("-c") + 1] = str(mutant)
        command = ["-O0" if arg.startswith("-O") else arg for arg in command]
        command.extend(["-iquote", str((REPO / source).parent)])

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

        compiled = execute(command, "compile.log")
        assert compiled.returncode == 0, (name, "compile", compiled.stderr[-3000:])
        command = link_command(args.build, target)
        if original in command:
            command[command.index(original)] = str(obj)
        else:
            library = "liblemonade-backend-definition-v2.a"
            assert library in command, (name, original)
            replacement = work / library
            shutil.copyfile(args.build / library, replacement)
            assert (
                execute(
                    ["ar", "r", str(replacement), str(obj)], "archive.log"
                ).returncode
                == 0
            )
            command[command.index(library)] = str(replacement)
        binary = work / "probe"
        command[command.index("-o") + 1] = str(binary)
        linked = execute(command, "link.log")
        assert linked.returncode == 0, (name, "link", linked.stderr[-3000:])
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        observed = execute(["timeout", "90", str(binary)], "run.log", timeout=100)
        assert observed.returncode == 1 and "FAIL " + assertion in observed.stdout, (
            name,
            observed.returncode,
            observed.stdout[-4000:],
            observed.stderr[-1000:],
        )
        record = dict(
            rule=name, assertion=assertion, compiled=True, exit=1, caught=True
        )
        results.append(record)
        (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
        print(record, flush=True)


if __name__ == "__main__":
    main()
