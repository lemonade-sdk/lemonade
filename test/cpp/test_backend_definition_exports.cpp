#include "backend_definition_export_fixture.h"
#include "llamacpp_external_fixture.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>

using namespace export_fixture;
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
bool has_code(const CompositionDiagnostics& result, const std::string& code) {
    return std::any_of(result.entries.begin(), result.entries.end(), [&](const auto& error) { return error.code == code; });
}
CompositionDiagnostics audit(const BuiltinExportContract& value, const std::vector<HookAbiDescriptor>& abis = installed_hook_abis()) {
    return v2::audit_export_contracts({value}, snapshot().snapshot, abis, v2::export_registry_digest({value}, abis));
}
void reject(const std::string& label, const std::string& code, const std::function<void(BuiltinExportContract&)>& change, bool repin = true) {
    auto value = contract();
    change(value);
    if (repin) export_fixture::repin(value);
    const auto result = audit(value);
    check(has_code(result, code), label);
    if (!has_code(result, code)) for (const auto& error : result.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
}
}

int main() {
    static_assert(!std::is_default_constructible_v<BuiltinExportRegistry>);
    check(installed_builtin_exports().exports().empty(), "production export set is empty");
    check(!installed_builtin_exports().find("llamacpp", 1), "llamacpp is not exported");
    const auto input = snapshot();
    check(input.snapshot != nullptr, "complete synthetic source snapshot");
    DefinitionResolver resolver(input.snapshot);
    const auto resolved = resolver.resolve({DefinitionNamespace::Builtin, "test-export", 1});
    check(resolved.definition != nullptr, "synthetic base resolves");
    if (!resolved.definition) {
        for (const auto& error : resolved.diagnostics.entries) std::printf("%s %s\n", error.code.c_str(), error.pointer.c_str());
        return 1;
    }
    const auto value = contract();
    check(resolved.definition->source_digest == value.expected_source_digest, "independent pinned source digest");
    check(resolved.definition->definition_hash == value.expected_definition_hash, "independent pinned declaration digest");
    check(resolved.definition->dependency_digest == value.expected_dependency_digest, "independent pinned dependency digest");
    const auto pins = Json::parse(read_bytes("export.synthetic.pins.json"));
    const auto result = v2::audit_export_contracts({value}, input.snapshot, installed_hook_abis(), pins.at("registry").get<std::string>());
    check(result.entries.empty() && !result.truncated, "complete pinned export audit");
    for (const auto& error : result.entries) std::printf("%s %s\n", error.code.c_str(), error.pointer.c_str());
    if (result.entries.empty()) {
        const auto registry = BuiltinCatalogLoader::test_exports({value}, input.snapshot, installed_hook_abis(), pins.at("registry").get<std::string>());
        check(registry->find("test-export", 1) != nullptr, "exact compiled test contract lookup");
        check(!registry->find("test-export", 2) && !registry->find("absent", 1), "no contract or external fallback");
        check(registry->digest() == pins.at("registry"), "registry preserves pinned identity");
    }
    reject("source pin", "source_digest_mismatch", [](auto& c) { c.expected_source_digest.back() ^= 1; });
    reject("declaration pin", "definition_digest_mismatch", [](auto& c) { c.expected_definition_hash.back() ^= 1; });
    reject("dependency pin", "dependency_digest_mismatch", [](auto& c) { c.expected_dependency_digest.back() ^= 1; });
    reject("policy pin", "policy_digest_mismatch", [](auto& c) { c.policy_digest.back() ^= 1; }, false);
    reject("mutation policy pin", "mutation_policy_digest_mismatch", [](auto& c) { c.slots[0].mutation_policy_digest.back() ^= 1; c.policy_digest = v2::export_policy_digest(c); }, false);
    check(has_code(v2::audit_export_contracts({value}, input.snapshot, installed_hook_abis(), "sha256:" + std::string(64, '0')), "registry_digest_mismatch"), "registry pin");
    reject("contract exact version", "invalid_export_source", [](auto& c) { c.key.contract = 2; });
    reject("contract positive", "invalid_export_key", [](auto& c) { c.key.contract = 0; });
    reject("policy revision positive", "invalid_export_key", [](auto& c) { c.policy_revision = 0; });
    reject("slot completeness", "uncovered_hook_slot", [](auto& c) { c.slots.pop_back(); });
    reject("slot ambiguity", "duplicate_hook_slot", [](auto& c) { c.slots.push_back(c.slots[0]); });
    reject("slot owner identity", "missing_export_slot", [](auto& c) { c.slots[1].slot.owner_id = "other"; });
    reject("slot stage identity", "missing_export_slot", [](auto& c) { c.slots[1].slot.stage = "launch"; });
    reject("hook ABI revision", "hook_abi_mismatch", [](auto& c) { ++c.slots[0].hook_abi_revision; });
    reject("hook parameters pin", "parameter_digest_mismatch", [](auto& c) { c.slots[0].parameter_digest.back() ^= 1; });
    reject("owner semantic pin", "owner_semantics_mismatch", [](auto& c) { c.slots[0].owner_semantics_digest.back() ^= 1; });
    reject("sealed hook ordering", "hook_order_mismatch", [](auto& c) { std::reverse(c.slots[2].ordered_hooks.begin(), c.slots[2].ordered_hooks.end()); });
    reject("root cannot remove with owner", "invalid_removal_policy", [](auto& c) { c.slots[0].removal = SlotRemoval::WithOwner; });
    reject("invalid removal enum", "invalid_removal_policy", [](auto& c) { c.slots[0].removal = static_cast<SlotRemoval>(99); });
    reject("missing slot dependency", "invalid_slot_dependency", [](auto& c) { c.slots[0].required_slots.push_back({SlotOwnerKind::Root, "other", "launch", "absent"}); });
    reject("cyclic slot dependencies", "slot_dependency_cycle", [](auto& c) { c.slots[0].required_slots.push_back(c.slots[1].slot); c.slots[1].required_slots.push_back(c.slots[0].slot); });
    reject("missing phase dependency", "invalid_phase_dependency", [](auto& c) { c.slots[0].required_phases.push_back("backend"); });
    reject("typed mutable shape", "invalid_data_policy", [](auto& c) { c.slots[0].mutable_inputs[0].schema_fragment = "arbitrary"; });
    reject("mutable type values", "invalid_data_policy", [](auto& c) { c.slots[0].mutable_inputs[0].values = {1}; });
    reject("mutable actual type", "invalid_mutable_input", [](auto& c) { c.slots[0].mutable_inputs[0].type = MutableValueType::Boolean; c.slots[0].mutable_inputs[0].maximum_length = 0; });
    reject("integer policy range exactness", "invalid_data_policy", [](auto& c) { auto& r = c.slots[0].mutable_inputs[0]; r.type = MutableValueType::Integer; r.maximum_length = 0; r.maximum = 1.5; });
    reject("nonfinite mutable enum", "invalid_data_policy", [](auto& c) { auto& r = c.slots[0].mutable_inputs[0]; r.target.path = "/metadata/config_extra/threshold"; r.type = MutableValueType::Number; r.maximum_length = 0; r.allow_absent = true; r.values = {std::numeric_limits<double>::quiet_NaN()}; });
    reject("mutable length bound", "invalid_mutable_input", [](auto& c) { c.slots[0].mutable_inputs[0].maximum_length = 1; });
    reject("mutable hook input", "invalid_data_policy", [](auto& c) { c.slots[0].mutable_inputs[0].target.path = "/hooks/launch/id"; });
    reject("mutable owner semantics", "mutable_owner_semantics", [](auto& c) { c.slots[0].mutable_inputs[0].target = {"/operations", RecordKey{RecordKey::Field::Id, "chat.buffered"}, "/operation"}; });
    reject("artifact owner semantic sealing", "mutable_owner_semantics", [](auto& c) { c.slots[0].mutable_inputs[0].target = {"/artifacts/main/role", {}, ""}; });
    reject("missing sealed field", "missing_policy_input", [](auto& c) { c.slots[0].sealed_inputs[0].path = "/metadata/absent"; });
    reject("duplicate mutable target", "invalid_data_policy", [](auto& c) { c.slots[0].mutable_inputs.push_back(c.slots[0].mutable_inputs[0]); });
    reject("overlapping mutable and sealed fields", "ambiguous_data_policy", [](auto& c) { c.slots[0].sealed_inputs.push_back({"/metadata", {}, ""}); });
    reject("effect contract required", "invalid_effect_policy", [](auto& c) { c.slots[0].effects.clear(); });
    reject("pure with side effects", "invalid_effect_policy", [](auto& c) { c.slots[0].effects.push_back(HookEffect::ProcessProbe); });
    auto abis = installed_hook_abis();
    abis.erase(abis.begin());
    check(has_code(audit(value, abis), "missing_hook_abi"), "missing installed hook");
    abis = installed_hook_abis(); abis[0].stage = "request";
    check(has_code(audit(value, abis), "hook_abi_mismatch"), "installed stage mismatch");
    abis = installed_hook_abis(); abis[0].operations = {"other"};
    check(has_code(audit(value, abis), "hook_abi_mismatch"), "installed operation restriction");
    abis = installed_hook_abis(); abis[0].revision = 2;
    check(has_code(audit(value, abis), "hook_abi_mismatch"), "installed ABI change requires new contract");
    abis = installed_hook_abis(); abis.push_back(abis[0]);
    check(has_code(audit(value, abis), "invalid_hook_abi"), "duplicate ABI metadata");
    abis = installed_hook_abis(); abis[0].operations = {"chat_completion", "chat_completion"};
    check(has_code(audit(value, abis), "invalid_hook_abi"), "ambiguous ABI operation restrictions");
    const auto inventory = Json::parse(read_bytes("hook-abi.inventory.json"));
    check(installed_hook_abis().size() == 19 && v2::hook_abi_json(installed_hook_abis()) == inventory.at("abis") &&
        installed_hook_abi_digest() == inventory.at("abi_digest"), "ReviewedTypedAbiInventory");
    check(v2::hook_abi_json(installed_legacy_hook_abis()) == inventory.at("legacy_abis"), "ExactLegacyAbiProjection");
    const auto* prepare = find_hook_abi("llamacpp.prepare.v1");
    check(prepare && prepare->stage == "launch" && prepare->revision == 1 && prepare->implementation_revision == 1 &&
        prepare->operations.empty(), "PrepareAbiUsesLaunchSlot");
    check(installed_builtin_exports().digest() == inventory.at("registry_digest"), "ReviewedProductionAggregatePin");
    auto stale_abis = installed_hook_abis();
    ++stale_abis.back().implementation_revision;
    check(has_code(v2::audit_export_contracts({}, nullptr, stale_abis, installed_builtin_exports().digest()),
        "registry_digest_mismatch"), "EmptyExportsStillPinAbiImplementation");
    stale_abis = installed_hook_abis();
    ++stale_abis.back().revision;
    check(has_code(v2::audit_export_contracts({}, nullptr, stale_abis, installed_builtin_exports().digest()),
        "registry_digest_mismatch"), "EmptyExportsStillPinAbiRevision");
    check(has_code(v2::audit_export_contracts({}, nullptr, installed_hook_abis(),
        "sha256:283179bf6e03aba5facd47fd9b2538007ecddc2ac9a46473573713694f070899"),
        "registry_digest_mismatch"), "EmptyExportsRejectPriorAggregatePin");
    std::set<std::string> hook_references;
    const auto walk = [&](const auto& self, const Json& rule, const std::string& path) -> void {
        if (rule.is_object() && rule.value("$ref", Json()) == "#/$defs/Hook") hook_references.insert(path);
        if (rule.is_object()) for (const auto& [key, field] : rule.items()) self(self, field, path + "/" + key);
        if (rule.is_array()) for (std::size_t i = 0; i < rule.size(); ++i) self(self, rule[i], path + "/" + std::to_string(i));
    };
    walk(walk, v2::schema_document(), "");
    std::set<std::string> expected_references = {"/$defs/Install/properties/hook", "/$defs/Artifact/properties/hook",
        "/$defs/Request/properties/hooks/items", "/$defs/Operation/properties/handler", "/$defs/Operation/properties/response/properties/hooks/items"};
    const std::vector<std::string> root_stages = {"install", "artifacts", "launch", "start_failure", "availability", "model_discovery", "model_download",
        "remote_context", "backend", "install_context", "runtime_options", "environment", "system"};
    for (const auto& stage : root_stages) expected_references.insert("/$defs/Hooks/properties/" + stage);
    check(hook_references == expected_references, "all schema Hook positions covered explicitly");
    auto covered = resolved.definition->declaration;
    covered["hooks"] = Json::object();
    const Json hook = {{"id", "coverage"}, {"params", Json::object()}};
    for (const auto& stage : root_stages) covered["hooks"][stage] = hook;
    covered["install"]["hook"] = hook;
    covered["artifacts"]["main"]["hook"] = hook;
    covered["operations"][0]["handler"] = hook;
    covered["operations"][0]["request"]["hooks"] = Json::array({hook});
    covered["operations"][0]["response"]["hooks"] = Json::array({hook});
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    auto selections = v2::hook_selections(covered, budget);
    check(selections.size() == 18, "every hook-bearing schema position enumerated");
    covered["operations"][0]["delivery"] = "streaming";
    selections = v2::hook_selections(covered, budget);
    check(std::any_of(selections.begin(), selections.end(), [](const auto& selection) { return selection.slot.stage == "sink_operation"; }), "streaming handler ABI stage");
    covered["operations"][0]["request"]["set"]["/payload"] = {{"literal", {{"hooks", Json::array({hook})}}}};
    check(v2::hook_selections(covered, budget).size() == 18, "literal payload hooks are ordinary data");
    MutableDataRule expression{{"/launch/program/missing_error", {}, ""}, MutableValueType::Expression};
    expression.schema_fragment = "Expression";
    expression.reference_roots = {"request"};
    Json expression_value = {{"ref", "/request/prompt"}, {"default", {{"ref", "/secret/inert-literal"}}}};
    check(v2::accepts_mutable_value(expression, &expression_value, budget), "expression reference default is literal data");
    expression_value = {{"ref", "/secret/value"}};
    check(!v2::accepts_mutable_value(expression, &expression_value, budget), "expression reference root restricted");
    expression_value = {{"unknown", true}};
    check(!v2::accepts_mutable_value(expression, &expression_value, budget), "audited expression schema required");
    check(!v2::accepts_mutable_value(expression, nullptr, budget), "mutable absence forbidden by default");
    expression.allow_absent = true;
    check(v2::accepts_mutable_value(expression, nullptr, budget), "explicit permitted absence");
    auto ambiguous_source = Json::parse(read_bytes("export.synthetic.json"));
    ambiguous_source["merge"]["hooks"]["install"] = {{"id", "first"}, {"params", Json::object()}};
    ambiguous_source["merge"]["install"]["hook"] = {{"id", "second"}, {"params", Json::object()}};
    const auto ambiguous_bytes = ambiguous_source.dump();
    const auto ambiguous_snapshot = make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "test-export", 1}, ambiguous_bytes, ""}});
    const auto ambiguous = v2::audit_export_contracts({value}, ambiguous_snapshot.snapshot, installed_hook_abis(), v2::export_registry_digest({value}, installed_hook_abis()));
    check(has_code(ambiguous, "ambiguous_hook_slot"), "ambiguous root slot ownership");
    auto external = Json::parse(read_bytes("export.synthetic.json"));
    external["id"] = external["recipe"] = "untrusted-parent";
    const auto external_bytes = external.dump();
    const Json promoted = {{"schema_version", 2}, {"id", "test-export"}, {"recipe", "test-export"}, {"extends", {{"external", "untrusted-parent"}}}};
    const auto promoted_bytes = promoted.dump();
    const auto promoted_snapshot = make_v2_source_snapshot({{{DefinitionNamespace::External, "untrusted-parent", 0}, external_bytes, ""},
        {{DefinitionNamespace::Builtin, "test-export", 1}, promoted_bytes, ""}});
    DefinitionResolver promoted_resolver(promoted_snapshot.snapshot);
    const auto promoted_result = promoted_resolver.resolve({DefinitionNamespace::Builtin, "test-export", 1});
    check(promoted_result.definition != nullptr, "namespace supplied to the pure resolver carries no authority");
    if (promoted_result.definition) {
        auto alleged = value;
        alleged.expected_source_digest = promoted_result.definition->source_digest;
        alleged.expected_definition_hash = promoted_result.definition->definition_hash;
        alleged.expected_dependency_digest = promoted_result.definition->dependency_digest;
        check(has_code(v2::audit_export_contracts({alleged}, promoted_snapshot.snapshot, installed_hook_abis(),
            v2::export_registry_digest({alleged}, installed_hook_abis())), "external_export_dependency"), "compiled exports cannot inherit external authority");
    }
    lemon::backends::external_fixture::export_checks(check);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
