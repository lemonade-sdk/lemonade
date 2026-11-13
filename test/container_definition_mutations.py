"""Require named assertion failures from independently compiled C2 rule mutants."""

import argparse
import concurrent.futures
import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"


def run(command, cwd, log):
    with log.open("w") as output:
        return subprocess.run(
            command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, timeout=180
        )


def mutations(schema):
    results = []

    def field(name, record, member, replacement, assertion):
        changed = copy.deepcopy(schema)
        changed["$defs"][record]["properties"][member] = replacement
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))

    field(
        "mutable-tag",
        "ContainerPolicy",
        "repository",
        {"type": "string"},
        "MutableRepositoryTagRejected",
    )
    results.append(
        (
            "namespace",
            "backend_definition_v2_contract.cpp",
            (
                "return text.size() <= 256 && std::regex_match(text, std::regex(pattern));",
                "return pattern == \"^[a-zA-Z0-9][a-zA-Z0-9._-]*$\" ? std::regex_match(text, std::regex(pattern)) : text.find(':', 10) == std::string::npos;",
            ),
            "UntrustedNamespaceRejected",
        )
    )
    field("mutable-pin", "ContainerPolicy", "pin", {}, "MutablePinRejected")
    field("pin-source", "ContainerPin", "source", {}, "ForeignPinSourceRejected")
    field(
        "device-domain",
        "ContainerPolicy",
        "devices",
        {"type": "array"},
        "UnknownDeviceRejected",
    )
    field(
        "capability-domain",
        "ContainerPolicy",
        "cap_add",
        {"type": "array"},
        "UnknownCapabilityRejected",
    )
    field("network", "ContainerPolicy", "network", {}, "HostNetworkRejected")
    field("confinement", "ContainerPolicy", "policy", {}, "SubstitutePolicyRejected")
    field("teardown", "ContainerPolicy", "teardown", {}, "UnknownTeardownRejected")
    field(
        "stop-grace",
        "ContainerPolicy",
        "stop_grace_seconds",
        {},
        "ChangedStopGraceRejected",
    )
    field("mount-destination", "Mount", "destination", {}, "ForeignDestinationRejected")
    field("mount-expansion", "Mount", "expand", {}, "UnknownExpansionRejected")
    for name, path, assertion in (
        (
            "managed-container-program",
            ["ExecutionCase", "oneOf", 1, "properties", "program"],
            "ManagedContainerProgramRejected",
        ),
        (
            "command-name",
            ["ImageProgram", "oneOf", 0, "properties", "name"],
            "ContainerCommandPathRejected",
        ),
        ("address", ["Launch", "properties", "address"], "HostAddressRejected"),
    ):
        changed = copy.deepcopy(schema)
        node = changed["$defs"]
        for key in path[:-1]:
            node = node[key]
        node[path[-1]] = {}
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))
    for name, path, assertion in (
        (
            "container-native-case",
            [
                "launch",
                "properties",
                "execution",
                "properties",
                "cases",
                "items",
                "properties",
                "kind",
            ],
            "ContainerNativeCaseSchemaRejected",
        ),
        (
            "dual-program",
            ["launch", "properties", "program"],
            "TopLevelManagedProgramRejected",
        ),
        ("container-install", ["install"], "ContainerManagedInstallRejected"),
    ):
        changed = copy.deepcopy(schema)
        node = changed["$defs"]["Declaration"]["oneOf"][1]["properties"]
        for key in path[:-1]:
            node = node[key]
        node[path[-1]] = {}
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))
    for name, record, assertion in (
        ("inline-pin", "ContainerPin", "InlinePinRejected"),
        ("arbitrary-host-mount", "Mount", "ArbitraryHostMountRejected"),
    ):
        changed = copy.deepcopy(schema)
        changed["$defs"][record]["additionalProperties"] = True
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))
    for name, old, new, assertion in (
        (
            "mount-inventory",
            '!artifacts.contains(mount["artifact"].get<std::string>())',
            "false",
            "ForeignArtifactMountRejected",
        ),
        (
            "selector-overlap",
            '!selected.emplace(backend.get<std::string>(), item["kind"].get<std::string>()).second',
            '(selected.emplace(backend.get<std::string>(), item["kind"].get<std::string>()), false)',
            "ExecutionOverlapRejected",
        ),
        (
            "support-case",
            '!selected.count(backend) || ((row["format"] == "container") != (selected[backend] == "container"))',
            "false",
            "ContainerSupportMismatchRejected",
        ),
    ):
        results.append(
            (name, "backend_definition_provenance.cpp", (old, new), assertion)
        )
    results.append(
        (
            "execution-binding",
            "backend_definition_binding.cpp",
            (
                "AuthorityInspection::permits_execution(token, program.normalized)",
                "true",
            ),
            "ContainerCannotReachNativeProcess",
        )
    )
    results.append(
        (
            "legacy-process-fallback",
            "launch_planner.cpp",
            (
                'definition.records.value("kind", "local") != "local" || definition.records.contains("containers") || launch.contains("execution")',
                "false",
            ),
            "LegacyPlannerCannotReachNativeProcess",
        )
    )
    results.append(
        (
            "external-container-authority",
            "backend_definition_authorization.cpp",
            (
                'builtin || (declaration.at("kind") == "local" && !declaration.contains("containers") &&\n            !declaration.at("launch").contains("execution"))',
                "true",
            ),
            "HookFreeExternalContainerHasNoExecutionAuthority",
        )
    )
    for name, filename, old, new, assertion in (
        (
            "execution-identity",
            "backend_definition_provenance.cpp",
            '"/launch/execution/cases", ',
            "",
            "DuplicateExecutionIdentityRejected",
        ),
        (
            "mount-identity",
            "backend_definition_provenance.cpp",
            '"/launch/mounts", ',
            "",
            "DuplicateMountIdentityRejected",
        ),
        (
            "native-install-identity",
            "backend_definition_provenance.cpp",
            '"/install/native/cases", ',
            "",
            "NativeInstallDuplicateIdentityRejected",
        ),
        (
            "execution-edit-registry",
            "backend_definition_edits.cpp",
            "const auto field = record_field(path);",
            'auto field = record_field(path); if (path == "/launch/execution/cases") field.reset();',
            "ContainerRecordsSupportStableEdits",
        ),
        (
            "mount-edit-registry",
            "backend_definition_edits.cpp",
            "const auto field = record_field(path);",
            'auto field = record_field(path); if (path == "/launch/mounts") field.reset();',
            "ContainerRecordsSupportStableEdits",
        ),
        (
            "readiness-upper-bound",
            "backend_definition_provenance.cpp",
            "timeout > 86400",
            "false",
            "UnboundedReadinessRejected",
        ),
        (
            "oci-exact-record",
            "backend_definition_provenance.cpp",
            'strategy == "oci" && installation.size() != 1',
            "false",
            "ContainerDefaultVersionsRejected",
        ),
        (
            "inherited-environment",
            "backend_definition_provenance.cpp",
            'launch["environment"]["preset"] != "inherit"',
            "false",
            "NativeEnvironmentPresetRejected",
        ),
        (
            "inherited-directory",
            "backend_definition_provenance.cpp",
            'launch["working_directory"] != "inherit"',
            "false",
            "NativeWorkingDirectoryRejected",
        ),
        (
            "policy-count",
            "backend_definition_provenance.cpp",
            "policies.size() > 32",
            "false",
            "TooManyPoliciesRejected",
        ),
        (
            "unused-policy",
            "backend_definition_provenance.cpp",
            "!used.count(name)",
            "false",
            "UnusedPolicyRejected",
        ),
        (
            "container-reference",
            "backend_definition_provenance.cpp",
            'const auto name = item["container"].get<std::string>();',
            'const auto name = std::string("rocm");',
            "MissingContainerReferenceRejected",
        ),
        (
            "backend-policy-reference",
            "backend_definition_provenance.cpp",
            "backend != name",
            "false",
            "CaseBackendPolicyMismatchRejected",
        ),
        (
            "readiness-path",
            "backend_definition_provenance.cpp",
            'launch["readiness"]["path"].get_ref<const std::string&>()[0] != \'/\'',
            "false",
            "ContainerReadinessPathRejected",
        ),
    ):
        results.append((name, filename, (old, new), assertion))
    for name, path, key, assertion in (
        (
            "execution-record-bound",
            ["Launch", "properties", "execution", "properties", "cases"],
            "maxItems",
            "ExecutionRecordBoundRejected",
        ),
        (
            "mount-record-bound",
            ["Launch", "properties", "mounts"],
            "maxItems",
            "MountRecordBoundRejected",
        ),
        (
            "backend-selector-bound",
            ["ExecutionCase", "oneOf", 1, "properties", "backends"],
            "maxItems",
            "BackendSelectorBoundRejected",
        ),
    ):
        changed = copy.deepcopy(schema)
        node = changed["$defs"]
        for key_path in path:
            node = node[key_path]
        del node[key]
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))
    for name, record, assertion in (
        ("execution-id-required", "ExecutionCase", "ExecutionStableIdRequired"),
        ("mount-id-required", "Mount", "MountStableIdRequired"),
    ):
        changed = copy.deepcopy(schema)
        record = changed["$defs"][record]
        if "oneOf" in record:
            record = record["oneOf"][1]
        record["required"].remove("id")
        results.append((name, "backend_definition_v2_contract.cpp", changed, assertion))
    changed = copy.deepcopy(schema)
    changed["$defs"]["ImageProgram"]["oneOf"][1]["additionalProperties"] = True
    results.append(
        (
            "entrypoint-record-closed",
            "backend_definition_v2_contract.cpp",
            changed,
            "EntrypointNameRejected",
        )
    )
    changed = copy.deepcopy(schema)
    changed["$defs"]["Launch"]["properties"]["readiness"]["properties"]["timeout"][
        "anyOf"
    ][-1]["properties"]["floor_seconds"]["minimum"] = 0
    results.append(
        (
            "readiness-floor-bound",
            "backend_definition_v2_contract.cpp",
            changed,
            "UnboundedReadinessFloorRejected",
        )
    )
    changed = copy.deepcopy(schema)
    changed["$defs"]["NativeInstall"]["properties"]["strategy"]["enum"].append("hook")
    changed["$defs"]["NativeInstall"]["properties"]["hook"] = {}
    results.append(
        (
            "native-install-no-hook",
            "backend_definition_v2_contract.cpp",
            changed,
            "NativeInstallHookNotAdmitted",
        )
    )
    changed = copy.deepcopy(schema)
    changed["$defs"]["ContainerPolicy"]["properties"]["repository"]["maxLength"] = 512
    results.append(
        (
            "repository-length",
            "backend_definition_v2_contract.cpp",
            changed,
            "RepositoryLengthBoundRejected",
        )
    )
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    for path in (args.scratch, args.out):
        path.mkdir(parents=True, exist_ok=True)
    args.build, args.scratch, args.out = (
        p.resolve() for p in (args.build, args.scratch, args.out)
    )
    temporary = args.scratch / "tmp"
    temporary.mkdir(exist_ok=True)
    os.environ.update(TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
    control = run(
        ["timeout", "60", str(args.build / "test_container_definition_v2")],
        REPO,
        args.out / "control.log",
    )
    assert control.returncode == 0, "GreenControlBeforeMutations"
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    link = subprocess.check_output(
        [
            "ninja",
            "-C",
            str(args.build),
            "-t",
            "commands",
            "test_container_definition_v2",
        ],
        text=True,
        timeout=30,
    ).splitlines()[-1]
    link = shlex.split(link.removeprefix(": && ").removesuffix(" && :"))
    schema = json.loads(
        (REPO / "schemas/backend-definition-v2.schema.json").read_bytes()
    )
    results = []
    hashes = {
        path: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in CONFIGURED.glob("*.cpp")
    }

    def probe(mutation):
        name, filename, change, assertion = mutation
        source = CONFIGURED / filename
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        row = next(
            r
            for r in database
            if Path(r["file"]) == source
            and (
                (
                    filename != "backend_definition_binding.cpp"
                    or "lemonade-binding-test.dir" in r["command"]
                )
                and (
                    filename != "backend_definition_authorization.cpp"
                    or "test_container_definition_v2.dir" in r["command"]
                )
            )
        )
        command = shlex.split(row["command"])
        command.insert(1, "-I" + str(CONFIGURED))
        original_object = command[command.index("-o") + 1]
        text = source.read_text()
        if isinstance(change, dict):
            encoded = json.dumps(change, separators=(",", ":"))
            literals = "\n".join(
                'R"c2schema(' + encoded[i : i + 6000] + ')c2schema"'
                for i in range(0, len(encoded), 6000)
            )
            text = text.replace(
                "nlohmann::json::parse(k_backend_definition_v2_schema)",
                "nlohmann::json::parse(" + literals + ")",
            )
        else:
            old, new = change
            assert (
                text.count(old) in (1, 2)
                if filename == "launch_planner.cpp"
                else text.count(old) == 1
            ), name
            text = text.replace(old, new)
        if name == "external-container-authority":
            text = text.replace(
                '!container_capable(definition.declaration), "container_execution_unavailable"',
                'true, "container_execution_unavailable"',
            )
        if name == "repository-length":
            text = text.replace("text.size() <= 256", "text.size() <= 512")
        mutant, obj, binary = (
            work / filename,
            work / "mutant.o",
            work / "test_container_definition_v2",
        )
        mutant.write_text(text)
        command = [
            str(mutant) if x == str(source) else str(obj) if x == original_object else x
            for x in command
        ]
        assert (
            run(
                command, Path(row["directory"]), args.out / f"{name}.compile.log"
            ).returncode
            == 0
        ), (name + ": compile failure is not a kill")
        linked = [str(obj) if x == original_object else x for x in link]
        if original_object not in link:
            linked.insert(linked.index("-o"), str(obj))
        linked[linked.index("-o") + 1] = str(binary)
        linked = [x for x in linked if not x.startswith("-Wl,--dependency-file=")]
        assert run(linked, args.build, args.out / f"{name}.link.log").returncode == 0, (
            name + ": link failure is not a kill"
        )
        log = args.out / f"{name}.run.log"
        result = run(["timeout", "60", str(binary)], REPO, log)
        caught = result.returncode == 1 and "FAIL " + assertion in log.read_text()
        assert caught, name
        return dict(
            rule=name, assertion=assertion, exit=result.returncode, caught=caught
        )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, min(args.jobs, 4))
    ) as pool:
        for result in pool.map(probe, mutations(schema)):
            results.append(result)
            (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(result["rule"], "caught", result["assertion"], flush=True)
    assert all(
        hashlib.sha256(path.read_bytes()).hexdigest() == digest
        for path, digest in hashes.items()
    ), "WorkingSourcesUnchangedByMutations"
    print(len(results), "independently compiled mutants caught")


if __name__ == "__main__":
    main()
