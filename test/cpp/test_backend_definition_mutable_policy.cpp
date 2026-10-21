#include "backend_definition_mutable_fixture.h"

#include <cstdio>
#include <functional>
#include <limits>
#include <set>

using namespace mutable_fixture;
namespace {
int checks = 0, failures = 0;
void check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
void expect(const Json& document, const std::string& code, const std::string& label, BuiltinExportContract p = policy()) {
    const auto result = authorize(document, std::move(p));
    const bool found = std::any_of(result.diagnostics.entries.begin(), result.diagnostics.entries.end(), [&](const auto& d) { return d.code == code; });
    check(code.empty() ? result.definition != nullptr : !result.definition && found, label);
    if ((code.empty() && !result.definition) || (!code.empty() && !found))
        for (const auto& d : result.diagnostics.entries) std::printf("  %s %s\n", d.code.c_str(), d.pointer.c_str());
}
Json collection(const std::string& op, const std::string& path, Json value = nullptr, Json match = nullptr) {
    auto result = child();
    Json item = {{"op", op}, {"path", path}};
    if (!value.is_null()) item["value"] = std::move(value);
    if (!match.is_null()) item["match"] = std::move(match);
    result["edits"] = Json::array({item});
    return result;
}
void audit_reject(const std::string& label, const std::string& code, const std::function<void(BuiltinExportContract&)>& change) {
    auto p = policy(); change(p); repin(p);
    const auto result = v2::audit_export_contracts({p}, snapshot("typed-authority.synthetic", "typed-authority").snapshot,
        installed_hook_abis(), v2::export_registry_digest({p}, installed_hook_abis()));
    check(std::any_of(result.entries.begin(), result.entries.end(), [&](const auto& d) { return d.code == code; }), label);
}
}

int main() {
    expect(child(), "", "PinnedTypedAuthorityAdmitted");
    auto no_hooks_policy = policy();
    for (auto& slot : no_hooks_policy.slots) slot.removal = SlotRemoval::Independent;
    repin(no_hooks_policy);
    auto removed = child();
    removed["edits"] = Json::array({
        {{"op", "clear"}, {"path", "/operations"}, {"match", {{"id", "chat.buffered"}}}, {"at", "/request/hooks"}},
        {{"op", "clear"}, {"path", "/operations"}, {"match", {{"id", "chat.buffered"}}}, {"at", "/response/hooks"}}
    });
    expect(removed, "", "LastHookRemovalWithinEnvelope", no_hooks_policy);
    removed["edits"].push_back({{"op", "replace"}, {"path", "/metadata/server_name"}, {"value", "escape"}});
    expect(removed, "fragment_scope_escape", "EnvelopeSurvivesLastSlotRemoval", no_hooks_policy);
    const auto p = policy();
    const auto pins = Json::parse(read_bytes("typed-authority.synthetic.pins.json"));
    check(v2::export_policy_digest(p) == pins.at("policy") && v2::export_registry_digest({p}, installed_hook_abis()) == pins.at("registry"), "TypedPolicyPinsAreLiteral");
    std::set<Kind> kinds; for (const auto& rule : p.mutable_fragments) kinds.insert(rule.kind);
    check(kinds.size() == 10, "TenClosedFragmentContracts");
    expect(edit("/metadata/options", "new_engine", "engine", "/name"), "", "OptionPresentationRename");
    expect(edit("/metadata/options", "--new-engine", "engine", "/cli_flag"), "", "OptionPresentationFlag");
    expect(edit("/metadata/options", "text hook literal", "engine", "/help"), "", "LiteralHookWordIsData");
    expect(edit("/metadata/options", 8, "threads", "/default_value"), "", "OptionDefaultInteger");
    expect(edit("/metadata/options", 32, "threads", "/maximum"), "", "OptionDefaultConstraint");
    expect(edit("/metadata/options", 4.5, "threads", "/default_value"), "invalid_mutable_fragment", "OptionIntegerDoesNotAcceptFraction");
    expect(edit("/metadata/options", "8", "threads", "/default_value"), "invalid_mutable_fragment", "OptionDefaultTypeSealed");
    expect(edit("/metadata/options", 20000, "threads", "/maximum"), "invalid_mutable_fragment", "OptionDefaultOuterRange");
    expect(edit("/metadata/options", -1, "threads", "/default_value"), "invalid_mutable_fragment", "OptionDefaultLowerRange");
    expect(edit("/metadata/options", "--bad flag", "engine", "/cli_flag"), "invalid_mutable_fragment", "OptionFlagGrammar");
    expect(edit("/metadata/options", "--", "engine", "/cli_flag"), "invalid_mutable_fragment", "EndOfOptionsIsNotFlag");
    expect(edit("/metadata/options", std::string(257, 'x'), "engine", "/name"), "invalid_mutable_fragment", "OptionStringBound");
    expect(edit("/metadata/options", Json::array({1, 2}), "threads", "/default_value"), "invalid_mutable_fragment", "StructuredDefaultRejected");
    auto renamed = edit("/launch/argv", "--fork-device", "device", "/flag");
    renamed["edits"].push_back({{"op", "replace"}, {"path", "/launch/custom_args/reserved"}, {"value", {"--fork-device", "-d"}}});
    expect(renamed, "", "ArgvSpellingsAndReservations");
    expect(edit("/launch/custom_args/reserved", Json::array({"--device", "-d", "--fork"})), "", "ReservedFlagsExtended");
    expect(edit("/launch/custom_args/reserved", Json::array({"--device"})), "required_reservation_removed", "RequiredReservationCannotDisappear");
    expect(edit("/launch/custom_args/reserved", Json::array({"--device", "-d", "-d"})), "invalid_mutable_fragment", "DuplicateReservationRejected");
    expect(edit("/launch/argv", "gpu", "device", "/value"), "", "ArgvDesignatedDataValue");
    expect(edit("/launch/argv", "never", "device", "/reserve"), "sealed_data_changed", "ReservationPolicySealed");
    auto custom = collection("add", "/launch/custom_args/defaults", {{"id", "fork"}, {"args", "--fork 2"}, {"flag", "--fork"}, {"stage", "runtime_options"}, {"respect_merge_args", false}, {"when", true}});
    expect(custom, "", "CustomDefaultsAdd");
    custom["edits"][0]["value"]["args"] = "--other 2";
    expect(custom, "invalid_mutable_fragment", "CustomDefaultsUseInstalledParser");
    custom["edits"][0]["value"]["args"] = "--fork 2 --device escaped";
    expect(custom, "custom_default_flag_escape", "CustomDefaultCannotHideAnotherFlag");
    custom["edits"][0]["value"]["args"] = "--fork 2"; custom["edits"][0]["value"]["when"] = "yes";
    expect(custom, "typed_reference_mismatch", "CustomDefaultConditionBoolean");
    expect(collection("remove", "/launch/custom_args/defaults", nullptr, {{"id", "parallel"}}), "", "CustomDefaultsRemove");
    expect(collection("clear", "/launch/custom_args/defaults"), "", "CustomDefaultsClear");
    expect(edit("/launch/backend/fixed", "vulkan"), "", "DeviceSelectionFiniteValue");
    expect(edit("/metadata/default_device", "gpu"), "", "DeviceDefaultFiniteValue");
    expect(edit("/metadata/rocm_channels", Json::array({"nightly"})), "", "DeviceChannelFiniteValue");
    expect(edit("/launch/backend/fixed", "remote-probe"), "mutable_domain_escape", "DeviceCannotGrantProbe");
    auto environment = collection("add", "/launch/environment/set", {{"name", "FORK_DATA"}, {"value", "hook data"}});
    expect(environment, "", "EnvironmentEntriesAdd");
    environment["edits"][0]["value"]["value"] = {{"ref", "/system/environment/HOME"}};
    expect(environment, "typed_reference_mismatch", "EnvironmentCannotReadParent");
    environment["edits"][0]["value"]["value"] = 1;
    expect(environment, "typed_reference_mismatch", "EnvironmentValueString");
    environment["edits"][0]["value"] = {{"name", "PATH"}, {"value", "x"}};
    expect(environment, "mutable_domain_escape", "EnvironmentNamesBounded");
    expect(collection("clear", "/launch/environment/set"), "", "EnvironmentEntriesClear");
    auto support = Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"]["metadata"]["support"][0];
    support["id"] = "gpu"; support["backend"] = "vulkan"; support["devices"] = {{"gpu", {"aarch64"}}};
    expect(collection("add", "/metadata/support", support), "", "SupportRowsAdd");
    support["devices"] = {{"arbitrary-probe", {"x86_64"}}};
    expect(collection("add", "/metadata/support", support), "mutable_domain_escape", "SupportDeviceDomain");
    support["devices"] = {{"gpu", {"aarch64"}}}; support["hook"] = {{"id", "llamacpp.system.v1"}};
    expect(collection("add", "/metadata/support", support), "unknown_key", "SupportCannotSmuggleHook");
    auto install = edit("/install/cases", "test/fork", "cpu", "/repo");
    expect(install, "", "InstallCasesForkRepository");
    install = edit("/install/cases", {{"concat", Json::array({"fork-", {{"ref", "/install/version"}}})}}, "cpu", "/filename");
    expect(install, "", "InstallCasesTypedAssetReference");
    install["edits"][0]["value"] = {{"ref", "/launch/executable"}};
    expect(install, "typed_reference_mismatch", "InstallCannotReadLaunchPhase");
    auto phase_policy = policy();
    phase_policy.mutable_fragments[8].references.push_back({"/launch/executable", Type::String, Phase::Launch});
    for (auto& slot : phase_policy.slots) slot.mutable_fragments = phase_policy.mutable_fragments;
    repin(phase_policy);
    expect(install, "typed_reference_mismatch", "ReferenceAvailabilityPhaseChecked", phase_policy);
    install["edits"][0]["value"] = {{"ref", "/install/version"}, {"type", "boolean"}};
    expect(install, "typed_reference_mismatch", "TypedReferenceCannotChangeProducerType");
    expect(edit("/install/default_versions", "v2", "", ""), "invalid_type", "InstallVersionsRemainMap");
    expect(edit("/install/default_versions/cpu", "v2"), "", "InstallDefaultVersionData");
    expect(collection("add", "/install/default_versions/outside", "v2"), "mutable_domain_escape", "InstallDefaultVersionBackendDomain");
    expect(edit("/metadata/options", "fork_args", "engine", "/name"), "option_collision", "OptionRenameCollision");
    expect(edit("/install/cases", "attacker/repo", "cpu", "/repo"), "mutable_domain_escape", "InstallRepositoryEnvelope");
    expect(edit("/launch/context_floor/embedding", 4096), "", "ContextFloorsChange");
    expect(collection("remove", "/launch/context_floor/embedding"), "", "ContextFloorsRemove");
    expect(edit("/launch/context_floor/embedding", 0), "invalid_mutable_fragment", "ContextFloorsPositive");
    expect(edit("/launch/context_floor", {{"unknown", 4096}}), "unknown_key", "ContextFloorsClosedKeys");
    expect(edit("/metadata/server_name", "escape"), "sealed_data_changed", "MutableScopeEscapeRejected");
    auto whole = Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"]["launch"];
    expect(edit("/launch", whole), "fragment_subtree_replacement", "ParentSubtreeReplacementRejected");
    auto option = Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"]["metadata"]["options"][0];
    expect(edit("/metadata/options", option, "engine"), "fragment_subtree_replacement", "RecordSubtreeReplacementRejected");
    expect(edit("/operations", "completion", "chat.buffered", "/operation"), "authored_owner_change", "OwnerSemanticsStaySealed");
    auto restricted = policy();
    restricted.slots[0].mutable_fragments[0].fields = {"help"}; repin(restricted);
    expect(edit("/metadata/options", "renamed", "engine", "/name"), "sealed_data_changed", "SlotIntersectionIsNotUnion", restricted);
    restricted = policy();
    restricted.slots[0].mutable_fragments[4].collection = {true, true, false}; repin(restricted);
    expect(collection("clear", "/launch/custom_args/defaults"), "collection_permission_denied", "SlotCollectionPermissionEnforced", restricted);
    auto bypass = collection("clear", "/launch/custom_args/defaults");
    bypass["edits"].push_back({{"op", "add"}, {"path", "/launch/custom_args/defaults"}, {"value", Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"]["launch"]["custom_args"]["defaults"][0]}});
    expect(bypass, "collection_permission_denied", "ClearReaddCannotBypassPermission", restricted);
    restricted = policy(); restricted.slots[0].mutable_fragments[4].collection = {false, true, true}; repin(restricted);
    auto transient = collection("add", "/launch/custom_args/defaults", {{"id", "transient"}, {"args", "--transient 1"}, {"flag", "--transient"}, {"stage", "launch"}, {"respect_merge_args", true}});
    transient["edits"].push_back({{"op", "remove"}, {"path", "/launch/custom_args/defaults"}, {"match", {{"id", "transient"}}}});
    expect(transient, "collection_permission_denied", "AddRemoveCannotBypassPermission", restricted);
    restricted = policy(); restricted.slots[0].mutable_fragments[4].collection = {true, false, true}; repin(restricted);
    transient = collection("remove", "/launch/custom_args/defaults", nullptr, {{"id", "parallel"}});
    transient["edits"].push_back({{"op", "add"}, {"path", "/launch/custom_args/defaults"}, {"value", Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"]["launch"]["custom_args"]["defaults"][0]}});
    expect(transient, "collection_permission_denied", "RemoveReaddCannotBypassPermission", restricted);
    restricted = policy(); restricted.mutable_fragments[6].required_records = {"GGML_DISABLE_VULKAN"};
    for (auto& slot : restricted.slots) slot.mutable_fragments = restricted.mutable_fragments;
    repin(restricted);
    expect(collection("clear", "/launch/environment/set"), "required_fragment_record_removed", "RequiredRecordSurvives", restricted);
    for (unsigned i = 0; i < 10; ++i) audit_reject("ClosedTarget:" + std::to_string(i), "invalid_mutable_fragment", [i](auto& c) {
        c.mutable_fragments[i].target = {"/launch", {}, ""};
    });
    {
        auto siblings = contract("export.siblings.synthetic", "test-siblings");
        MutableFragmentRule escape{Kind::OptionPresentationV1, {"/operations", RecordKey{RecordKey::Field::Id, "completion.streaming"}, ""}, {"operation"}};
        siblings.mutable_fragments = {escape};
        for (auto& slot : siblings.slots) slot.mutable_fragments = {escape};
        repin(siblings);
        const auto diagnostics = v2::audit_export_contracts({siblings}, snapshot("export.siblings.synthetic", "test-siblings").snapshot,
            installed_hook_abis(), v2::export_registry_digest({siblings}, installed_hook_abis()));
        check(std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [](const auto& d) { return d.code == "invalid_mutable_fragment"; }), "HookFreeOwnerCannotBeMutable");
    }
    audit_reject("SlotCannotWidenOuterEnvelope", "fragment_envelope_escape", [](auto& c) { ++c.slots[0].mutable_fragments[0].maximum_length; });
    audit_reject("TypedReferenceInventoryCannotLie", "typed_reference_mismatch", [](auto& c) { c.mutable_fragments[8].references[0].type = Type::Boolean; });
    audit_reject("TypedReferenceFieldClosed", "typed_reference_mismatch", [](auto& c) { c.mutable_fragments[8].references[0].path = "/install/arbitrary"; });
    audit_reject("TypedReferencePhasePinned", "typed_reference_mismatch", [](auto& c) { c.mutable_fragments[8].references[0].phase = Phase::Input; });
    audit_reject("UnknownFragmentKind", "invalid_mutable_fragment", [](auto& c) { c.mutable_fragments[0].kind = static_cast<Kind>(99); });
    audit_reject("FragmentHardRecordCeiling", "invalid_mutable_fragment", [](auto& c) { c.mutable_fragments[0].maximum_records = std::numeric_limits<std::size_t>::max(); });
    audit_reject("FragmentHardStringCeiling", "invalid_mutable_fragment", [](auto& c) { c.mutable_fragments[0].maximum_length = std::numeric_limits<std::size_t>::max(); });
    {
        const auto source = Json::parse(read_bytes("typed-authority.synthetic.json"))["merge"];
        auto doc = source;
        doc["hooks"]["launch"] = {{"id", "llamacpp.prepare.v1"}};
        MutableFragmentRule rule{Kind::EnvironmentEntriesV1, {"/launch/environment/set", {}, ""}};
        rule.domains = {{"name", {"GGML_DISABLE_VULKAN"}}};
        rule.references = {{"/launch/hook/context_size", Type::Integer, Phase::Prepare}};
        doc["launch"]["environment"]["set"][0]["value"] = {{"string", {{"ref", "/launch/hook/context_size"}}}};
        DefinitionLimits limits;
        std::size_t retained = 0;
        v2::Budget budget{limits, {}, retained};
        v2::validate_fragments({rule}, doc, doc, budget);
        check(true, "TypedLaunchProducerReference");
        doc.erase("hooks");
        bool rejected = false;
        try { v2::validate_fragments({rule}, doc, source, budget); }
        catch (const v2::AuthorizationFailure& failure) { rejected = failure.code == "typed_reference_mismatch"; }
        check(rejected, "RemovedFactProducerRejected");
        const auto artifact = v2::typed_data_reference("/artifacts/main", doc);
        check(!artifact, "UndeclaredArtifactFactRejected");
        doc["artifacts"]["main"] = {{"role", "main"}};
        check(v2::typed_data_reference("/artifacts/main", doc).has_value() && !v2::typed_data_reference("/artifacts/main/arbitrary", doc), "ArtifactReferenceIsNamed");
        limits.work_nodes = 0;
        v2::Budget exhausted{limits, {}, retained};
        rejected = false;
        try { v2::validate_fragments({rule}, doc, source, exhausted); } catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected, "FragmentWorkChargedBeforeTraversal");
        doc = source;
        rule.references.clear();
        limits = {};
        limits.retained_logical_bytes = 4096;
        retained = 0;
        v2::Budget small_storage{limits, {}, retained};
        rejected = false;
        try { v2::validate_fragments({rule}, doc, source, small_storage); }
        catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected, "FragmentRetentionPreflight");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
