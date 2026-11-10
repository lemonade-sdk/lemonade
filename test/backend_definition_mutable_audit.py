"""Compile isolated mutable-policy and admission-ticket mutations."""

import argparse
import json
from pathlib import Path
import shlex

from llamacpp_oracle_audit import compile_source, link_command, run

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
MUTABLE = "test_backend_definition_mutable_policy"
ADMISSION = "test_backend_catalog_admission"


def mutations():
    cases = []

    def add(name, file, before, after, assertion, target=MUTABLE, count=1):
        source = (CONFIGURED / file).read_text()
        assert source.count(before) == count, (name, source.count(before))
        cases.append((name, target, file, source.replace(before, after), assertion))

    policy = "backend_definition_policy.cpp"
    auth = "backend_definition_authorization.cpp"
    add(
        "mutable_scope_escape",
        auth,
        "require(data_projection(",
        "require(true || data_projection(",
        "MutableScopeEscapeRejected",
        count=2,
    )
    add(
        "parent_subtree_replacement",
        policy,
        'at.empty() || path.rfind(at + "/", 0) == 0',
        "false",
        "ParentSubtreeReplacementRejected",
    )
    add(
        "record_subtree_replacement",
        policy,
        "                edit != DefinitionEditKind::Merge) throw AuthorizationFailure",
        "                false) throw AuthorizationFailure",
        "RecordSubtreeReplacementRejected",
    )
    add(
        "typed_reference_confusion",
        policy,
        'require_policy(!expression.contains("type") || expression.at("type") == type_name(known->type), "typed_reference_mismatch");\n        require_policy(!expression.contains("default") || scalar_type(expression.at("default"), known->type), "typed_reference_mismatch");\n        return known->type;',
        "return known->type;",
        "TypedReferenceCannotChangeProducerType",
        count=2,
    )
    add(
        "collection_clear_permission",
        policy,
        '            if (edit == DefinitionEditKind::Clear) require_policy(rule.collection.clear, "collection_permission_denied");',
        "            if (edit == DefinitionEditKind::Clear) (void)rule.collection.clear;",
        "ClearReaddCannotBypassPermission",
    )
    add(
        "slot_intersection_bypass",
        auth,
        "mask_fragments(result, policy.mutable_fragments, budget);",
        "mask_fragments(result, contract.mutable_fragments, budget);",
        "SlotIntersectionIsNotUnion",
    )
    add(
        "outer_envelope_bypass",
        policy,
        "rule.maximum_length <= outer->maximum_length",
        "true",
        "SlotCannotWidenOuterEnvelope",
    )
    add(
        "missing_fact_producer",
        policy,
        'if (selected("launch", "llamacpp.prepare.v1"))',
        "if (true)",
        "RemovedFactProducerRejected",
    )
    add(
        "reservation_omitted_alias",
        policy,
        'for (const auto& flag : optional_array(argv, "aliases")) require_policy(std::find(value->begin(), value->end(), flag) != value->end(), "required_reservation_removed");',
        "",
        "RequiredReservationCannotDisappear",
    )
    add(
        "device_domain_escape",
        policy,
        "else check(*value);",
        "else (void)*value;",
        "DeviceCannotGrantProbe",
    )
    add(
        "custom_default_parser_bypass",
        policy,
        '!tokens.empty() && tokens.front() == record.at("flag")',
        "true",
        "CustomDefaultsUseInstalledParser",
    )
    add(
        "environment_parent_read",
        policy,
        '        expression(record.at("value"), Type::String, rule, doc, Phase::Launch, budget);',
        "(void)record;",
        "EnvironmentCannotReadParent",
    )
    add(
        "support_device_domain_escape",
        policy,
        'domain(rule, "device", device);',
        "(void)device;",
        "SupportDeviceDomain",
    )
    add(
        "install_repo_escape",
        policy,
        'domain(rule, "repo", record.at("repo"));',
        "(void)record;",
        "InstallRepositoryEnvelope",
    )
    add(
        "context_floor_zero",
        policy,
        "floor > 0 && floor >= rule.minimum",
        "floor >= rule.minimum",
        "ContextFloorsPositive",
    )
    add(
        "required_record_removal",
        policy,
        'for (const auto& required : rule.required_records) require_policy(identities.count(required), "required_fragment_record_removed");',
        "",
        "RequiredRecordSurvives",
    )
    add(
        "default_type_escape",
        policy,
        "require_policy(scalar_type(value, expected));",
        "(void)expected;",
        "OptionIntegerDoesNotAcceptFraction",
    )
    add(
        "string_bound_escape",
        policy,
        "text.size() <= rule.maximum_length && text.find",
        "text.find",
        "OptionStringBound",
    )
    add(
        "nonclosed_fragment_target",
        policy,
        "require_policy(rule.target.path == path && rule.target.match && rule.target.match->field == RecordKey::Field::Id && !rule.target.match->value.empty());",
        'if (rule.target.path == "/operations") continue;\n        require_policy(rule.target.path == path && rule.target.match && rule.target.match->field == RecordKey::Field::Id && !rule.target.match->value.empty());',
        "HookFreeOwnerCannotBeMutable",
    )
    add(
        "last_slot_envelope_bypass",
        auth,
        "if (contract && !contract->mutable_fragments.empty()) {",
        "if (false) {",
        "EnvelopeSurvivesLastSlotRemoval",
    )
    add(
        "hidden_custom_default_flag",
        policy,
        'for (const auto& [flag, values] : arguments) require_policy(flag == record.at("flag"), "custom_default_flag_escape");',
        "(void)arguments;",
        "CustomDefaultCannotHideAnotherFlag",
    )
    add(
        "collection_add_permission",
        policy,
        'else if (edit == DefinitionEditKind::Add) require_policy(rule.collection.add, "collection_permission_denied");',
        "else if (edit == DefinitionEditKind::Add) (void)rule.collection.add;",
        "AddRemoveCannotBypassPermission",
    )
    add(
        "collection_remove_permission",
        policy,
        '            if (edit == DefinitionEditKind::Remove) require_policy(rule.collection.remove, "collection_permission_denied");',
        "            if (edit == DefinitionEditKind::Remove) (void)rule.collection.remove;",
        "RemoveReaddCannotBypassPermission",
    )
    add(
        "end_of_options_flag",
        policy,
        "if (prefix == text.size() || !letter(text[prefix])) return false;",
        "if (false) return false;",
        "EndOfOptionsIsNotFlag",
    )
    add(
        "ticket_check_removed",
        "backend_catalog_admission.cpp",
        "if (!catalog.impl_ || !token.ticket_ || token.ticket_.get() != catalog.impl_->ticket.get()) return false;",
        "if (!catalog.impl_) return false;",
        "TicketIdentityRequiredForListedToken",
        ADMISSION,
    )
    add(
        "exact_membership_removed",
        "backend_catalog_admission.cpp",
        "    for (const auto& entry : catalog.impl_->builtin_v2) if (entry.second.get() == &token) return true;\n    for (const auto& entry : catalog.impl_->v2) if (entry.second.get() == &token) return true;\n    return false;",
        "    return true;",
        "UnlistedTokenRejectedDespiteMatchingTicket",
        ADMISSION,
    )
    add(
        "typed_reference_phase_escape",
        policy,
        "static_cast<unsigned>(known->phase) <= static_cast<unsigned>(use)",
        "true",
        "ReferenceAvailabilityPhaseChecked",
    )
    source = (CONFIGURED / policy).read_text()
    start = source.index("void bounded_value(")
    end = source.index("\nbool scalar_type(", start)
    body = source[start:end]
    for charge in [
        "    budget.keep(checked_product(checked_sum(sizeof(Json) + 64, bytes), 16));\n",
        "            budget.keep(checked_product(checked_sum(key.size(), 64), 16));\n",
    ]:
        assert body.count(charge) == 1
        body = body.replace(charge, "")
    cases.append(
        (
            "fragment_retention_unaccounted",
            MUTABLE,
            policy,
            source[:start] + body + source[end:],
            "FragmentRetentionPreflight",
        )
    )
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", action="append", default=[])
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out = args.out.resolve()
    assert args.out.is_relative_to(
        REPO / ".synagent/scratch"
    ), "Use persistent project scratch"
    args.out.mkdir(parents=True, exist_ok=True)
    database = json.loads((args.build / "compile_commands.json").read_text())
    results = []
    for name, target, file, source, assertion in mutations():
        if args.only and name not in args.only:
            continue
        folder = args.out / name
        folder.mkdir(exist_ok=True)
        resources = folder / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        mutant = folder / file
        mutant.write_text(source)
        owner = (
            "lemonade-backend-definition-v2"
            if file == "backend_definition_policy.cpp"
            else (
                "lemonade-server-core"
                if file == "backend_catalog_admission.cpp"
                else target
            )
        )
        entry = next(
            e
            for e in database
            if e["file"] == str(CONFIGURED / file) and owner + ".dir" in e["command"]
        )
        obj = folder / "mutation.o"
        compiled = run(
            compile_source(entry, mutant, obj, folder) + ["-O0"],
            args.build,
            folder / "compile.log",
        )
        assert compiled.returncode == 0, f"Did not compile: {name}"
        command = link_command(args.build, target)
        original = shlex.split(entry["command"])
        old_obj = original[original.index("-o") + 1]
        if old_obj in command:
            command = [str(obj) if x == old_obj else x for x in command]
        else:
            assert owner == "lemonade-backend-definition-v2"
            command.insert(1, str(obj))
        binary = folder / "test"
        command[command.index("-o") + 1] = str(binary)
        linked = run(command, args.build, folder / "link.log")
        assert linked.returncode == 0, f"Did not link: {name}"
        observed = run([str(binary)], args.build, folder / "run.log")
        caught = observed.returncode != 0 and "FAIL " + assertion in observed.stdout
        results.append(
            dict(
                rule=name,
                assertion=assertion,
                compiled=True,
                linked=True,
                returncode=observed.returncode,
                caught=caught,
            )
        )
        (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"{name}: {'caught' if caught else 'MISSED'} by {assertion}", flush=True)
    assert results and all(item["caught"] for item in results)
    print(f"{len(results)}/{len(results)} compiled mutations caught")


if __name__ == "__main__":
    main()
