"""Mutation-check the audited operation-owner boundary in isolated scratch files."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess

from llamacpp_oracle_audit import compile_source, link_command, run

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
TARGET = "test_backend_definition_owner_graph"


def mutations():
    cases = []

    def add(name, file, before, after, assertion, arguments=()):
        text = (CONFIGURED / file).read_text()
        assert text.count(before) == 1, (name, text.count(before))
        cases.append((name, file, text.replace(before, after), assertion, arguments))

    auth = "backend_definition_authorization.cpp"
    exports = "backend_definition_exports.cpp"
    add(
        "inherited_sibling_denied",
        auth,
        'if (surviving.count(operation.at("id").get<std::string>())) continue;',
        'if (surviving.count(operation.at("id").get<std::string>())) { '
        "for (const auto& protected_owner : contract.owners) "
        'if (operation.at("id") != protected_owner.id) '
        'require(!operation_owners_collide(operation, protected_owner.semantics, budget), "protected_owner_recreated", source); continue; }',
        "InheritedSameKindSiblingAdmitted",
    )
    add(
        "hook_free_sibling_denied",
        auth,
        "if (!current) continue;",
        'if (!current) continue; require(!policy.request_hooks.empty() || !policy.response_hooks.empty() || !policy.handler_hooks.empty(), "protected_owner_recreated", source);',
        "InheritedHookFreeCompletionAdmitted",
    )
    add(
        "new_alias_admitted",
        auth,
        'require(!operation_owners_collide(operation, protected_owner.semantics, budget), "protected_owner_recreated", source);',
        'require(true, "protected_owner_recreated", source);',
        "NewHooklessAliasRejected",
    )
    add(
        "zero_grants_drop_owner_obligations",
        auth,
        "if (contract) verify_operation_owners(definition, parent, *contract, selected, parent_root, current_root, budget);",
        "if (contract && !selected.empty()) verify_operation_owners(definition, parent, *contract, selected, parent_root, current_root, budget);",
        "AliasObligationsSurviveZeroGrants",
    )
    add(
        "hidden_hook_free_owner_write_ignored",
        auth,
        'require(path[2] != field, "authored_owner_change", source);',
        'require(true, "authored_owner_change", source);',
        "HiddenHookFreeOwnerWriteThenRemovalRejected",
    )
    add(
        "copied_sanitizer_not_inspected",
        auth,
        'require(!hook_position(path), "authored_hook_selection", source);',
        'require(true, "authored_hook_selection", source);',
        "CopiedSanitizerNewOwnerRejected",
    )
    add(
        "hook_free_owner_unaccounted",
        exports,
        'if (!covered.count(operation.at("id").get<std::string>())) fail("uncovered_operation_owner");',
        'if (false) fail("uncovered_operation_owner");',
        "HookFreeOwnerRecordRequired",
    )
    add(
        "duplicate_owner_admitted",
        exports,
        'if (!covered.insert(owner.id).second) fail("duplicate_operation_owner");',
        '(void)covered.insert(owner.id); if (false) fail("duplicate_operation_owner");',
        "OwnerIdentityUnique",
    )
    add(
        "nonexistent_owner_admitted",
        exports,
        'if (original == operations.end()) { fail("missing_operation_owner"); continue; }',
        "if (original == operations.end()) continue;",
        "OwnerRecordMustExist",
    )
    add(
        "remove_readd_laundering",
        auth,
        'require(owner(parent.declaration, slot, budget) != nullptr, "owner_reintroduced", source);',
        'require(true, "owner_reintroduced", source);',
        "FinalImmediateParentOwnerSubset",
    )
    add(
        "owner_provenance_ignored",
        auth,
        'if (a != before->object.end()) verify_provenance(a->second, b->second, source, budget, "owner_provenance_changed");',
        'if (false) verify_provenance(a->second, b->second, source, budget, "owner_provenance_changed");',
        "FinalHookFreeOwnerProvenance",
    )
    add(
        "owner_semantics_ignored",
        auth,
        'require(operation_owner_semantics(*current, budget) == policy.semantics, "owner_semantics_changed", source);',
        'require(true, "owner_semantics_changed", source);',
        "FinalHookFreeOwnerSemantics",
    )
    add(
        "owner_removal_permission_ignored",
        auth,
        'require(policy.allow_removal || owner(definition.declaration, {SlotOwnerKind::Operation, policy.id, "", ""}, budget),',
        "require(true,",
        "FinalHookFreeRemovalPermission",
    )
    add(
        "owner_dependency_ignored",
        auth,
        'require(owner(definition.declaration, {SlotOwnerKind::Operation, dependency, "", ""}, budget) != nullptr, "required_owner_removed", source);',
        'require(true, "required_owner_removed", source);',
        "RetainedOwnerDependencyCannotDisappear",
    )
    add(
        "final_owner_chain_ignored",
        auth,
        'require(operation_hook_chain(*current, stage, budget) == expected, "owner_chain_changed", source);',
        'require(true, "owner_chain_changed", source);',
        "FinalOwnerChainMustMatchGraph",
    )
    add(
        "missing_owner_dependency_admitted",
        exports,
        "std::none_of(contract.owners.begin(), contract.owners.end(), [&](const auto& item) { return item.id == dependency; })",
        "false",
        "OwnerDependencyMustExist",
    )
    add(
        "duplicate_owner_dependency_admitted",
        exports,
        "!dependencies.insert(dependency).second",
        "(dependencies.insert(dependency), false)",
        "OwnerDependencyUnique",
    )
    add(
        "self_owner_dependency_admitted",
        exports,
        "dependency == owner.id ||",
        "false ||",
        "OwnerCannotDependOnItself",
    )
    add(
        "owner_dependency_cycle_admitted",
        exports,
        "if (!active.insert(owner.id).second) return false;",
        "if (!active.insert(owner.id).second) return true;",
        "OwnerDependenciesAcyclic",
    )
    add(
        "nonremovable_owner_slot_mismatch",
        exports,
        "(slot.removal == SlotRemoval::WithOwner && !owner.allow_removal)",
        "false",
        "OwnerSlotRemovalConsistent",
    )
    add(
        "forbidden_slot_owner_mismatch",
        exports,
        "(slot.removal == SlotRemoval::Forbidden && owner.allow_removal)",
        "false",
        "ForbiddenSlotMakesOwnerNonremovable",
    )
    add(
        "owner_semantics_copy_uncharged",
        exports,
        "budget.keep(checked_sum(field.size(), checked_sum(metrics.bytes, checked_product(metrics.nodes, sizeof(Json)))));",
        "(void)metrics;",
        "OwnerSemanticsChargedBeforeCopy",
    )
    add(
        "owner_chain_work_uncharged",
        exports,
        "        budget.work(id.size(), 1);\n",
        "(void)id;",
        "OwnerChainWorkChargedBeforeRetention",
    )
    add(
        "owner_graph_pin_ignored",
        exports,
        "if (owner_graph_digest(contract.owners) != contract.owner_graph_digest)",
        "if (false)",
        "IndependentOwnerGraphPin",
    )
    add(
        "empty_export_registry_pin_ignored",
        exports,
        "if (!has_digest(expected_registry_digest) || export_registry_digest(contracts, abis) != expected_registry_digest)",
        "if (false)",
        "EmptyExportRegistryStillPinsAbiMetadata",
    )
    for field, assertion in [
        ("request", "EmptyRequestChainAudited"),
        ("response", "EmptyResponseChainAudited"),
        ("handler", "EmptyHandlerChainAudited"),
    ]:
        add(
            f"{field}_chain_unaudited",
            exports,
            f'owner.{field}_hooks != operation_hook_chain(*original, "{field}", budget)',
            "false",
            assertion,
        )
    for name, before, assertion in [
        (
            "fields",
            "owner.sealed_fields != operation_owner_fields()",
            "OwnerSealedFieldsComplete",
        ),
        ("collisions", "owner.collisions != collisions", "CollisionGraphComplete"),
        (
            "semantics",
            "owner.semantics != operation_owner_semantics(*original, budget)",
            "OwnerSemanticsAudited",
        ),
    ]:
        add(f"{name}_unaudited", exports, before, "false", assertion)
    add(
        "mutable_hook_free_owner",
        exports,
        'if (targets_overlap(rule.target, {"/operations", RecordKey{RecordKey::Field::Id, owner.id}, "/" + field}, limits)) return true;',
        "if (false) return true;",
        "HookFreeOwnerCannotBeMutable",
    )
    add(
        "envelope_changes_endpoint_identity",
        exports,
        'return a.at("method") == b.at("method")',
        'return a.at("kind") == b.at("kind") && a.at("method") == b.at("method")',
        "EndpointIdentityIgnoresEnvelopeAndDelivery",
        ("--endpoints",),
    )
    add(
        "public_operation_kind_ignored",
        exports,
        'if (left.at("operation") == right.at("operation")) return true;',
        "if (false) return true;",
        "PublicOperationKindConservative",
        ("--endpoints",),
    )
    add(
        "uncertain_endpoint_exempted",
        exports,
        "if (!known_endpoint(left, budget) || !known_endpoint(right, budget)) return true;",
        "if (!known_endpoint(left, budget) || !known_endpoint(right, budget)) return false;",
        "UncertainEndpointHasNoExemption",
        ("--endpoints",),
    )
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["mutations"])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--run-only", action="store_true")
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out = args.out.resolve()
    assert not args.out.is_relative_to(REPO), "Mutations must stay outside the tree"
    args.out.mkdir(parents=True, exist_ok=True)
    if args.run_only:
        manifest = json.loads((args.out / "mutation-builds.json").read_text())
        test_mutations(args, manifest)
        return
    database = json.loads((args.build / "compile_commands.json").read_text())
    manifest = []
    for name, file, text, assertion, arguments in mutations():
        folder = args.out / name
        folder.mkdir(exist_ok=True)
        source = folder / file
        source.write_text(text)
        entry = next(
            row
            for row in database
            if row["file"] == str(CONFIGURED / file)
            and TARGET + ".dir" in row["command"]
        )
        output = folder / "mutation.o"
        command = compile_source(entry, source, output, folder) + ["-O0"]
        compiled = run(command, args.build, folder / "compile.log")
        assert compiled.returncode == 0, f"Mutant did not compile: {name}"
        original = shlex.split(entry["command"])
        object_path = original[original.index("-o") + 1]
        command = link_command(args.build, TARGET)
        command = [str(output) if arg == object_path else arg for arg in command]
        binary = folder / "test"
        command[command.index("-o") + 1] = str(binary)
        linked = run(command, args.build, folder / "link.log")
        assert linked.returncode == 0, f"Mutant did not link: {name}"
        manifest.append(
            {
                "rule": name,
                "assertion": assertion,
                "compiled": True,
                "binary": str(binary),
                "arguments": arguments,
            }
        )
        (args.out / "mutation-builds.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        print(f"{name}: compiled and linked", flush=True)
    if not args.build_only:
        test_mutations(args, manifest)


def test_mutations(args, manifest):
    for item in manifest:
        name, assertion = item["rule"], item["assertion"]
        tested = run(
            [item["binary"], *item["arguments"]],
            args.build,
            args.out / name / "run.log",
        )
        caught = tested.returncode != 0 and f"FAIL {assertion}" in tested.stdout
        item.update(caught=caught, returncode=tested.returncode)
        (args.out / "mutations.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"{name}: {'caught' if caught else 'MISSED'} by {assertion}", flush=True)
    assert all(item["caught"] for item in manifest), "Mutants escaped their assertions"
    print(f"{len(manifest)}/{len(manifest)} isolated owner mutations caught")


if __name__ == "__main__":
    main()
