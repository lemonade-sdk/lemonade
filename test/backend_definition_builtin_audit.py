"""Mutation-check builtin minting and ABI pins in isolated scratch objects."""

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

from llamacpp_oracle_audit import compile_source, link_command, run

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
BUILTIN = "test_backend_definition_builtin_authorization"
EXPORTS = "test_backend_definition_exports"


def mutations():
    cases = []

    def add(name, target, file, before, after, assertion):
        source = (CONFIGURED / file).read_text()
        assert source.count(before) == 1, (name, source.count(before))
        cases.append((name, target, file, source.replace(before, after), assertion))

    auth = "backend_definition_authorization.cpp"
    exports = "backend_definition_exports.cpp"
    add(
        "public_builtin_origin_forged",
        BUILTIN,
        auth,
        "return authorize_accounted(leaf, nullptr);",
        "return authorize_impl(leaf, nullptr, leaf.space == DefinitionNamespace::Builtin);",
        "PublicAuthorizerRejectsBuiltinOrigin",
    )
    add(
        "public_intake_budget_exception_escapes",
        BUILTIN,
        auth,
        "try { return compose(builtins.snapshot, external_sources, limits, retained); }",
        "return compose(builtins.snapshot, external_sources, limits, retained); "
        "try { return compose(builtins.snapshot, external_sources, limits, retained); }",
        "ExternalCompositionBudgetReturnsDiagnostic",
    )
    add(
        "builtin_policy_leaks_to_external_child",
        BUILTIN,
        auth,
        "const auto* contract = frozen.registry->find(key.id, key.contract);",
        "const auto* contract = frozen.registry->find(key.id, key.contract); "
        "if (!contract && frozen.builtin_policies) for (const auto& item : frozen.builtin_policies->leaves) "
        "if (item.key.id == key.id && item.key.contract == key.contract) contract = &item;",
        "ExternalChildCannotUseInternalBuiltinPolicy",
    )
    add(
        "builtin_policy_audit_skipped",
        BUILTIN,
        auth,
        "if (builtin_policies) {",
        "if (false && builtin_policies) {",
        "BuiltinSourcePinChecked",
    )
    add(
        "builtin_sources_admitted_as_external",
        BUILTIN,
        auth,
        "source.key.space != DefinitionNamespace::Builtin || source.key.contract == 0",
        "false",
        "BuiltinTableRejectsExternalSources",
    )
    add(
        "builtin_accessor_ignores_exact_major",
        BUILTIN,
        "backend_catalog_admission.cpp",
        "const auto found = impl_->builtin_v2.find(key_string(key));",
        "const auto found = impl_->builtin_v2.begin();",
        "BuiltinAccessorRequiresExactNamespaceAndMajor",
    )
    for field, assertion in [
        ("source", "BuiltinSourcePinChecked"),
        ("definition", "BuiltinDefinitionPinChecked"),
        ("dependency", "BuiltinDependencyPinChecked"),
    ]:
        member = "definition_hash" if field == "definition" else field + "_digest"
        expected = "definition_hash" if field == "definition" else member
        add(
            field + "_pin_unchecked",
            BUILTIN,
            exports,
            f"if (definition.{member} != contract.expected_{expected})",
            "if (false)",
            assertion,
        )
    add(
        "builtin_policy_pin_unchecked",
        BUILTIN,
        exports,
        "if (!has_digest(contract.policy_digest) || export_policy_digest(contract) != contract.policy_digest)",
        "if (false)",
        "BuiltinPolicyPinChecked",
    )
    add(
        "builtin_and_external_aggregate_pin_unchecked",
        BUILTIN,
        exports,
        "if (!has_digest(expected_registry_digest) || export_registry_digest(contracts, abis) != expected_registry_digest)",
        "if (false)",
        "BuiltinAggregatePinChecked",
    )
    add(
        "empty_exports_skip_abi_repin",
        EXPORTS,
        exports,
        "if (!has_digest(expected_registry_digest) || export_registry_digest(contracts, abis) != expected_registry_digest)",
        "if (!contracts.empty() && (!has_digest(expected_registry_digest) || export_registry_digest(contracts, abis) != expected_registry_digest))",
        "EmptyExportsRejectPriorAggregatePin",
    )
    add(
        "typed_implementation_revision_omitted",
        EXPORTS,
        exports,
        '{"implementation_revision", descriptor.implementation_revision}',
        '{"implementation_revision", descriptor.id.find(".v1") == std::string::npos ? descriptor.implementation_revision : 1}',
        "EmptyExportsStillPinAbiImplementation",
    )
    add(
        "builtin_truncation_becomes_success",
        BUILTIN,
        auth,
        "if (!builtin_audit.entries.empty() || builtin_audit.truncated) return result;",
        "if (!builtin_audit.entries.empty()) return result;",
        "TruncatedBuiltinAuditIsFailure",
    )
    add(
        "builtin_mint_skipped_at_admission",
        BUILTIN,
        "backend_catalog_admission.cpp",
        "for (const auto& key : BuiltinCatalogLoader::builtin_leaves(*snapshot->authority_)) {",
        "for (const auto& key : std::vector<DefinitionKey>{}) {",
        "BuiltinTokenFreshForEveryAdmission",
    )
    add(
        "abi_audit_invokes_target",
        "test_backend_hook_abi",
        "backend_hook_registry.cpp",
        "const auto stage = static_cast<std::size_t>(registration.stage);",
        "const auto stage = static_cast<std::size_t>(registration.stage); "
        "if (registration.stage == HookStage::Request) "
        "std::get<RequestHook>(registration.target)(nlohmann::json::object());",
        "ABI audit never invokes executable targets",
    )
    add(
        "legacy_accepts_typed_artifact_provider",
        "test_backend_hook_abi",
        "backend_hook_registry.cpp",
        "const auto* abi = found == legacy.end() ? nullptr : &*found;",
        "const auto* abi = find_hook_abi(registration.id);",
        "TypedArtifactCannotImpersonateLegacyProvider",
    )
    return cases


def symbols(args):
    archive = args.build / "liblemonade-backend-definition-v2.a"
    defined = subprocess.check_output(
        ["nm", "-C", "--defined-only", archive], text=True
    )
    assert not re.search(
        r"BuiltinCatalogLoader::test_|CatalogAdmissionBuilder::test_", defined
    )
    objects = [
        args.build
        / "CMakeFiles/lemonade-backend-definition-v2.dir"
        / CONFIGURED.relative_to(REPO)
        / (name + ".o")
        for name in [
            "backend_definition_authorization.cpp",
            "backend_builtin_authorization.cpp",
        ]
    ]
    undefined = subprocess.check_output(
        ["nm", "-C", "-u", *map(str, objects)], text=True
    )
    forbidden = re.compile(
        r"installed_backend_hooks|llamacpp::|BackendOps|WrappedServer|ServerProcess|"
        r"builtin_registrations|httplib|curl_|\b(?:getenv|system|popen|socket|open|read|write)\b"
    )
    assert not any(forbidden.search(line) for line in undefined.splitlines())
    (args.out / "production-defined-symbols.txt").write_text(defined)
    (args.out / "loader-undefined-symbols.txt").write_text(undefined)
    print(
        "Production has no test constructors; loader has no hook, factory, IO or service dependencies"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["mutations", "symbols"])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", action="append", default=[])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--run-only", action="store_true")
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out = args.out.resolve()
    assert not args.out.is_relative_to(REPO), "Mutations must stay outside the tree"
    args.out.mkdir(parents=True, exist_ok=True)
    if args.mode == "symbols":
        symbols(args)
        return
    if args.run_only:
        manifest = json.loads((args.out / "mutation-builds.json").read_text())
    else:
        database = json.loads((args.build / "compile_commands.json").read_text())
        manifest = []
        for name, target, file, text, assertion in mutations():
            if args.only and name not in args.only:
                continue
            folder = args.out / name
            folder.mkdir(exist_ok=True)
            source = folder / file
            source.write_text(text)
            entry = (
                next(
                    row
                    for row in database
                    if row["file"] == str(CONFIGURED / file)
                    and target + ".dir" in row["command"]
                )
                if file != "backend_catalog_admission.cpp"
                and file != "backend_hook_registry.cpp"
                else next(
                    row
                    for row in database
                    if row["file"] == str(CONFIGURED / file)
                    and "lemonade-server-core.dir" in row["command"]
                )
            )
            output = folder / "mutation.o"
            compiled = run(
                compile_source(entry, source, output, folder) + ["-O0"],
                args.build,
                folder / "compile.log",
            )
            assert compiled.returncode == 0, f"Mutant did not compile: {name}"
            original = shlex.split(entry["command"])
            object_path = original[original.index("-o") + 1]
            command = link_command(args.build, target)
            assert object_path in command, f"Missing replacement object: {name}"
            command = [str(output) if arg == object_path else arg for arg in command]
            binary = folder / "test"
            command[command.index("-o") + 1] = str(binary)
            linked = run(command, args.build, folder / "link.log")
            assert linked.returncode == 0, f"Mutant did not link: {name}"
            manifest.append(
                dict(rule=name, assertion=assertion, compiled=True, binary=str(binary))
            )
            (args.out / "mutation-builds.json").write_text(
                json.dumps(manifest, indent=2) + "\n"
            )
            print(name + ": compiled and linked", flush=True)
    if args.build_only:
        return
    assert manifest, "No mutations selected"
    for item in manifest:
        result = run([item["binary"]], args.build, args.out / item["rule"] / "run.log")
        item.update(
            caught=result.returncode != 0
            and "FAIL " + item["assertion"] in result.stdout,
            returncode=result.returncode,
        )
        (args.out / "mutations.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(
            f"{item['rule']}: {'caught' if item['caught'] else 'MISSED'} by {item['assertion']}",
            flush=True,
        )
    assert all(item["caught"] for item in manifest), "Mutants escaped their assertions"
    print(f"{len(manifest)}/{len(manifest)} isolated builtin and ABI mutations caught")


if __name__ == "__main__":
    main()
