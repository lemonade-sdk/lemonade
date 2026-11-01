"""Compile C3 rule mutations and require their named failing CPU assertions."""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import threading

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
RUN_LOCK = threading.Lock()


def rules():
    result = []

    def add(name, file, old, new, assertion, suite="test_container_execution"):
        result.append((name, file, [(old, new)], assertion, suite))

    add(
        "execution-without-grant",
        "backend_definition_binding.cpp",
        "AuthorityInspection::permits_execution(token, program.normalized)",
        "true",
        "ContainerExecutionWithoutGrantRejected",
    )
    result.append(
        (
            "hook-free-external-execution",
            "backend_definition_authorization.cpp",
            [
                (
                    'builtin || (declaration.at("kind") == "local" && !declaration.contains("containers") &&\n            !declaration.at("launch").contains("execution"))',
                    "true",
                ),
                (
                    '!container_capable(definition.declaration), "container_execution_unavailable"',
                    'true, "container_execution_unavailable"',
                ),
            ],
            "HookFreeExternalCannotExecute",
            "test_container_execution",
        )
    )
    add(
        "owner-erased-with-hooks",
        "backend_definition_authorization.cpp",
        "    authority->grants.clear();",
        "    authority->grants.clear();\n    authority->execution_owner.reset();",
        "SealedExecutionOwnerIndependentOfHookGrants",
    )
    add(
        "owner-checked-only-with-hooks",
        "backend_definition_authorization.cpp",
        "owner->semantics == container_execution_owner(declaration) &&",
        "(authority.grants.empty() || owner->semantics == container_execution_owner(declaration)) &&",
        "HookRemovalCannotEraseSealedExecutionOwner",
    )
    add(
        "reviewed-grant-rebound",
        "backend_definition_authorization.cpp",
        "owner.semantics == container_execution_owner(declaration) &&",
        "true &&",
        "BuiltinCannotChangeReviewedExecutionOwner",
    )
    for path in (
        "/kind",
        "/containers/rocm/repository",
        "/containers/rocm/pin/recipe",
        "/containers/rocm/devices",
        "/containers/rocm/cap_add",
        "/containers/rocm/policy",
        "/containers/rocm/network",
        "/containers/rocm/teardown",
        "/containers/rocm/stop_grace_seconds",
        "/launch/execution/cases/0/program/source",
        "/launch/execution/cases/0/backends",
        "/launch/backend/fixed",
        "/launch/mounts/0/artifact",
        "/launch/mounts/0/destination",
        "/launch/address/source",
        "/launch/environment/preset",
        "/launch/working_directory",
        "/install/strategy",
        "/artifacts/main/role",
    ):
        semantic_path = (
            path.removeprefix("/launch") if path.startswith("/launch/") else path
        )
        add(
            "unsealed-" + path[1:].replace("/", "-"),
            "backend_definition_authorization.cpp",
            "owner->semantics == container_execution_owner(declaration)",
            "[&] { auto actual = container_execution_owner(declaration); "
            f'const nlohmann::json::json_pointer path("{semantic_path}"); '
            "actual[path] = owner->semantics.at(path); return owner->semantics == actual; }()",
            "SealedExecutionOwner:" + path,
        )
    add(
        "runner-without-authority",
        "backend_definition_runner.cpp",
        "!AuthorityInspection::permits_execution(executable_->compiled().authorized(), program_.normalized)",
        "false",
        "ContainerUnauthorizedRunnerRejectsBeforeServices",
        "test_container_definition_v2",
    )
    add(
        "mutable-pin",
        "container_execution.cpp",
        "!utils::parse_container_pin(effective)",
        "false",
        "MutableTagCannotReachLaunch",
    )
    add(
        "override-validation-bypass",
        "container_execution.cpp",
        "!utils::parse_container_pin(effective)",
        '(!utils::parse_container_pin(effective) && (requested.empty() || requested == "builtin"))',
        "PinOverrideCannotBypassValidation",
    )
    add(
        "override-without-pin-source",
        "container_execution.cpp",
        '    if (policy.pin.source != "backend_versions"',
        '    if (!requested.empty() && requested != "builtin") return requested;\n    if (policy.pin.source != "backend_versions"',
        "MissingPinSourceRejectsOverride",
    )
    add(
        "mutable-image-plan",
        "backend_definition_runner.cpp",
        "!valid_container_image(policy.repository, plan.container->image)",
        "false",
        "MutableImageRejectedBeforeInstallation",
    )
    add(
        "foreign-mount-inventory",
        "container_execution.cpp",
        'artifact == program.artifacts.end() || !frame.data.values.at("artifacts").contains(mount.artifact)',
        "false",
        "MountOutsideInventoryRejected",
    )
    add(
        "ambiguous-case",
        "container_execution.cpp",
        'if (selected) throw std::invalid_argument("Ambiguous execution case");',
        'if (false) throw std::invalid_argument("Ambiguous execution case");',
        "TwoMatchingExecutionCasesRejected",
    )
    add(
        "missing-case-fallback",
        "container_execution.cpp",
        'if (!selected) throw std::invalid_argument("Missing execution case");',
        "if (!selected) return program.launch.execution_cases.front();",
        "MissingExecutionCaseRejected",
    )
    add(
        "case-without-policy",
        "container_execution.cpp",
        'if (selected->kind == "container") container_policy(program, *selected);',
        "if (false) container_policy(program, *selected);",
        "ContainerCaseWithoutPolicyRejected",
    )
    add(
        "container-native-fallback",
        "configured_server.cpp",
        "        if (plan.container) {",
        "        if (false) {",
        "ContainerDefinitionActuallyExecutes",
    )
    add(
        "mount-rewrite-removed",
        "container_execution.cpp",
        '        frame.data.values["artifacts"][mount.artifact] = target;',
        "",
        "InventoryMountAndWholeValueRewrite",
    )
    add(
        "protected-environment",
        "backend_definition_runner.cpp",
        'name == "HOME" || name == "PATH" || name == "LD_PRELOAD" || name == "LD_LIBRARY_PATH" ||\n                name == "HIP_VISIBLE_DEVICES" || name == "ROCR_VISIBLE_DEVICES" || name == "CUDA_VISIBLE_DEVICES"',
        "false",
        "ProtectedContainerEnvironmentRejected",
    )
    add(
        "non-linux-container",
        "backend_definition_runner.cpp",
        'host.os != "linux"',
        "false",
        "ContainerLaunchRequiresLinux",
    )
    add(
        "oci-foreign-owner",
        "container_execution.cpp",
        'spec.effective_public_recipe() != program.normalized.at("recipe")',
        "false",
        "OCIInstallationRejectsForeignOwner",
    )
    add(
        "shard-completeness",
        "backend_artifact_services.cpp",
        "shards.size() != count || !shards.count(selected)",
        "false",
        "IncompleteShardInventoryRejected",
    )
    add(
        "shard-index",
        "backend_artifact_services.cpp",
        "!index || index > count || !shards.insert(file).second",
        "!shards.insert(file).second",
        "InvalidShardIndexRejected",
    )
    add(
        "foreign-shard",
        "backend_artifact_services.cpp",
        "match[1] != main[1] || ",
        "",
        "ForeignShardCannotFillInventory",
    )
    add(
        "oci-offline-fetch",
        "src/cpp/server/backend_manager.cpp",
        "config->offline() || config->no_fetch_executables()",
        "config->no_fetch_executables()",
        "OCIInstallationHonors:offline",
    )
    add(
        "oci-no-fetch",
        "src/cpp/server/backend_manager.cpp",
        "config->offline() || config->no_fetch_executables()",
        "config->offline()",
        "OCIInstallationHonors:no_fetch_executables",
    )
    add(
        "oci-host-substitution",
        "src/cpp/server/backends/backend_utils.cpp",
        "if (configured_container_policy(spec, backend)) throw",
        "if (false) throw",
        "OCIHasNoHostExecutableSubstitution",
    )
    add(
        "oci-managed-install-fallback",
        "backend_definition_runner.cpp",
        "services.install_container_image(plan.container->image);",
        "services.install_backend(spec.effective_public_recipe(), backend);",
        "ContainerDefinitionActuallyExecutes",
    )
    add(
        "oci-managed-program-fallback",
        "backend_definition_runner.cpp",
        'plan.command.program = program_source == "image_command" ? execution_case->program.at("name").get<std::string>() : "";',
        "plan.command.program = services.managed_binary(spec, backend);",
        "ContainerDefinitionActuallyExecutes",
    )
    add(
        "oci-external-program-fallback",
        "backend_definition_runner.cpp",
        'plan.command.program = program_source == "image_command" ? execution_case->program.at("name").get<std::string>() : "";',
        "plan.command.program = services.external_binary(spec, backend);",
        "ContainerDefinitionActuallyExecutes",
    )
    add(
        "readiness-floor-ignored",
        "backend_definition_runner.cpp",
        'std::max(services.default_timeout_seconds(), readiness["timeout"].at("floor_seconds").get<long>())',
        "services.default_timeout_seconds()",
        "ContainerReadinessHonorsGlobalFloor",
    )
    add(
        "native-install-branch-lost",
        "backend_execution_program.cpp",
        'doc["install"]["strategy"] == "by_execution" ? doc["install"]["native"] : doc["install"]',
        'doc["install"]',
        "MixedReleaseNativeInstallProgramRetained",
        "test_container_definition_v2",
    )
    add(
        "native-install-expression-path",
        "backend_execution_program.cpp",
        '"/install/native/cases/" : "/install/cases/"',
        '"/install/cases/" : "/install/cases/"',
        "MixedReleaseCompilesData",
        "test_container_definition_v2",
    )
    add(
        "materialized-destination-traversal",
        "backend_artifact_services.cpp",
        "target.lexically_normal() != target || ",
        "",
        "MaterializedDestinationTraversalRejected",
    )
    return result


def run(command, cwd, log):
    with log.open("w") as output:
        return subprocess.run(
            command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, timeout=180
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--only", nargs="+")
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
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    links = {}
    for suite in ("test_container_execution", "test_container_definition_v2"):
        commands = subprocess.check_output(
            ["ninja", "-t", "commands", suite], cwd=args.build, text=True
        ).splitlines()
        links[suite] = shlex.split(
            commands[-1].removeprefix(": && ").removesuffix(" && :")
        )
        control = run(
            ["timeout", "60", str(args.build / suite)],
            REPO,
            args.out / (suite + ".control.log"),
        )
        assert control.returncode == 0, "Control must pass"
    hashes = {
        p: hashlib.sha256(p.read_bytes()).hexdigest() for p in CONFIGURED.glob("*.cpp")
    }

    def probe(rule):
        name, filename, changes, assertion, suite = rule
        source = REPO / filename if "/" in filename else CONFIGURED / filename
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        selector = (
            suite + ".dir"
            if filename == "backend_definition_authorization.cpp"
            else (
                "lemonade-binding-test.dir"
                if filename
                in ("backend_definition_binding.cpp", "backend_execution_program.cpp")
                else "lemonade-server-core.dir"
            )
        )
        row = next(
            r
            for r in database
            if Path(r["file"]) == source and selector in r["command"]
        )
        command = shlex.split(row["command"])
        command.insert(1, "-I" + str(CONFIGURED))
        command.insert(1, "-I" + str(source.parent))
        old_object = command[command.index("-o") + 1]
        text = source.read_text()
        for old, new in changes:
            assert text.count(old) == 1, (name, text.count(old))
            text = text.replace(old, new)
        mutant, obj, binary = work / source.name, work / "mutant.o", work / suite
        mutant.write_text(text)
        command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in command
        ]
        if "-MF" in command:
            command[command.index("-MF") + 1] = str(work / "mutant.d")
        assert (
            run(
                command, Path(row["directory"]), args.out / (name + ".compile.log")
            ).returncode
            == 0
        ), (name + ": compile error is not a kill")
        link = [str(obj) if x == old_object else x for x in links[suite]]
        if old_object not in links[suite]:
            link.insert(link.index("-o"), str(obj))
        link[link.index("-o") + 1] = str(binary)
        link = [x for x in link if not x.startswith("-Wl,--dependency-file=")]
        assert run(link, args.build, args.out / (name + ".link.log")).returncode == 0, (
            name + ": link error is not a kill"
        )
        log = args.out / (name + ".run.log")
        with RUN_LOCK:
            result = run(["timeout", "60", str(binary)], REPO, log)
        caught = result.returncode == 1 and "FAIL " + assertion in log.read_text()
        assert caught, (name, result.returncode, assertion)
        print(name + ": caught by " + assertion, flush=True)
        return {
            "rule": name,
            "source": str(source.relative_to(REPO)),
            "changes": changes,
            "assertion": assertion,
            "exit": result.returncode,
            "caught": caught,
        }

    selected = [rule for rule in rules() if not args.only or rule[0] in args.only]
    assert selected and (not args.only or len(selected) == len(set(args.only)))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(probe, selected):
            results.append(result)
            (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    assert all(
        hashlib.sha256(p.read_bytes()).hexdigest() == digest
        for p, digest in hashes.items()
    )
    print(f"{len(results)}/{len(results)} compiled C3 mutations caught")


if __name__ == "__main__":
    main()
