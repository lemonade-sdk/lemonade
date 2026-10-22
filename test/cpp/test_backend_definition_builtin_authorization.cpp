#include "catalog_admission_fixture.h"
#include "lemon/backends/configured/backend_definition_execution.h"

#include <functional>
#include <type_traits>

using namespace admission_fixture;

namespace {

const DefinitionKey leaf{DefinitionNamespace::Builtin, "builtin-leaf", 1};
const DefinitionKey base{DefinitionNamespace::Builtin, "test-export", 1};

BuiltinExportContract leaf_policy() {
    auto policy = contract("builtin.leaf.synthetic", "builtin-leaf");
    policy.slots[0].slot.hook_id = "llamacpp.prepare.v1";
    return policy;
}

BuiltinExecutionPolicies policies() {
    const auto pins = Json::parse(read_bytes("builtin.leaf.synthetic.pins.json"));
    return {{leaf_policy()}, pins.at("registry")};
}

FrozenDefinitionSnapshotResult freeze(const BuiltinExecutionPolicies& policy = policies(),
    const DefinitionLimits& limits = {}) {
    const auto parent = read_bytes("export.synthetic.json");
    const auto child = read_bytes("builtin.leaf.synthetic.json");
    return BuiltinCatalogLoader::test_freeze_builtins({{base, parent, ""}, {leaf, child, ""}}, policy, limits);
}

std::shared_ptr<const FrozenBuiltinCatalog> builtin_catalog(std::shared_ptr<const FrozenDefinitionSnapshot> frozen) {
    const auto parent = read_bytes("export.synthetic.json");
    const auto child = read_bytes("builtin.leaf.synthetic.json");
    return CatalogAdmissionBuilder::test_builtins(std::move(frozen), {{base, parent, ""}, {leaf, child, ""}});
}

void rejected_policy(const std::string& label, const std::string& code,
    const std::function<void(BuiltinExecutionPolicies&)>& change) {
    auto policy = policies();
    change(policy);
    const auto rejected = freeze(policy);
    check(!rejected.snapshot && has_code(rejected.diagnostics, code), label);
}

}

int main() {
    static_assert(!std::is_default_constructible_v<AuthorizedDefinition>);
    static_assert(!std::is_constructible_v<AuthorizedDefinition, ResolvedDefinition>);
    static_assert(!std::is_constructible_v<AuthorizedDefinition, DefinitionKey>);
    static_assert(!std::is_constructible_v<ResolvedAuthority, ResolvedDefinition>);
    check(embedded_v2_sources().size() == 1 && embedded_v2_policies().leaves.size() == 1 &&
        embedded_v2_sources()[0].key.id == "llamacpp" && embedded_v2_policies().leaves[0].key.id == "llamacpp", "ProductionBuiltinV2TableExactlyLlama");
    const auto production = BuiltinCatalogLoader::freeze_embedded();
    check(production.snapshot && BuiltinCatalogLoader::builtin_leaves(*production.snapshot).size() == 1 &&
        BuiltinCatalogLoader::builtin_leaves(*production.snapshot)[0].id == "llamacpp", "ProductionMintsOnlyReviewedLlamaLeaf");
    const auto frozen = freeze();
    check(bool(frozen.snapshot), "LoaderAuditsPinnedBuiltinClosure");
    if (!frozen.snapshot) {
        for (const auto& error : frozen.diagnostics.entries) std::printf("%s %s\n", error.code.c_str(), error.pointer.c_str());
        return 1;
    }
    check(installed_builtin_exports().exports().empty(), "BuiltinPolicyDoesNotExportBase");
    const auto builtin = builtin_catalog(frozen.snapshot);
    const auto first = admit({}, builtin), second = admit({}, builtin);
    check(first.catalog && second.catalog, "BuiltinLeafAdmittedWithoutExport");
    if (!first.catalog || !second.catalog) return 1;
    const auto a = first.catalog->builtin_v2_definition(leaf), b = second.catalog->builtin_v2_definition(leaf);
    check(a && b && a.get() != b.get() && &a->authority() != &b->authority(), "BuiltinTokenFreshForEveryAdmission");
    if (!a || !b) return 1;
    check(first.catalog->metrics().authorized_definitions == 1 &&
        CatalogAdmissionBuilder::authority_identity(*a) == CatalogAdmissionBuilder::authority_identity(*b),
        "DeterministicIdentityDoesNotShareTokenObject");
    auto copy = a->definition();
    copy.declaration["hooks"]["launch"]["id"] = "forged";
    check(a->definition().declaration.at("hooks").at("launch").at("id") == "llamacpp.prepare.v1",
        "CallerConstructedResolvedDataCannotChangeAuthority");
    check(a->definition().key.space == DefinitionNamespace::Builtin && a->definition().dependencies.size() == 1 &&
        a->definition().dependencies[0].document.space == DefinitionNamespace::Builtin, "BuiltinClosureHasOnlyEmbeddedSources");
    check(a->definition().declaration.at("hooks").at("launch").at("id") == "llamacpp.prepare.v1",
        "BuiltinLeafSelectsOwnAuditedSlot");
    check(!first.catalog->builtin_v2_definition(base), "InternalBaseWithoutLeafPolicyCannotMint");
    check(!first.catalog->v2_definition(leaf.id) && !first.catalog->builtin_v2_definition({DefinitionNamespace::External, leaf.id, 0}) &&
        !first.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, leaf.id, 2}), "BuiltinAccessorRequiresExactNamespaceAndMajor");
    const auto forged = DefinitionAuthorizer(frozen.snapshot).authorize(leaf);
    check(!forged.definition && has_code(forged.diagnostics, "origin_promotion"), "PublicAuthorizerRejectsBuiltinOrigin");
    const auto raw = load_v2_authority_snapshot({{leaf, read_bytes("builtin.leaf.synthetic.json"), ""}});
    check(!raw.snapshot && has_code(raw.diagnostics, "origin_promotion"), "ExternalIntakeCannotSelectBuiltinNamespace");
    auto child = Json{{"schema_version", 2}, {"id", "external-child"}, {"recipe", "external-child"},
        {"extends", {{"builtin", leaf.id}, {"contract", 1}}}};
    const auto external = admit({child}, builtin);
    check(!external.catalog && has_code(external.diagnostics, "unexported_builtin"), "ExternalChildCannotUseInternalBuiltinPolicy");
    child["extends"]["builtin"] = base.id;
    const auto external_base = admit({child}, builtin);
    check(!external_base.catalog && has_code(external_base.diagnostics, "unexported_builtin"), "InternalBaseAlsoRequiresExternalExport");
    const auto ordinary = admit({root("ordinary")}, builtin);
    check(ordinary.catalog && ordinary.catalog->v2_definition("ordinary") &&
        ordinary.catalog->builtin_v2_definition(leaf).get() != a.get(), "ExternalAdmissionPreservesSeparateBuiltinTokens");
    const auto candidate = validate_candidate_definition(first.catalog, root("candidate").dump());
    check(candidate.admission.catalog && candidate.admission.catalog->builtin_v2_definition(leaf).get() != a.get() &&
        first.catalog->builtin_v2_definition(leaf).get() == a.get() && !first.catalog->v2_definition("candidate"),
        "CandidateCannotModifyActiveBuiltinAuthority");
    const auto standalone = DefinitionAuthorizer(load_v2_authority_snapshot({
        {{DefinitionNamespace::External, "ordinary", 0}, root("ordinary").dump(), ""}}).snapshot)
        .authorize({DefinitionNamespace::External, "ordinary", 0});
    check(standalone.definition && standalone.definition->definition().key.space == DefinitionNamespace::External,
        "OrdinaryExternalAuthorityRemainsExternal");
    const auto compiled = compile_execution_definition(a);
    check(!compiled.definition, "BuiltinAuthorityDoesNotBypassCompilerSupport");
    auto missing = policies();
    missing.leaves.clear();
    missing.digest = v2::export_registry_digest({}, installed_hook_abis());
    const auto unregistered = freeze(missing);
    check(unregistered.snapshot && BuiltinCatalogLoader::builtin_leaves(*unregistered.snapshot).empty(), "SourcesAloneDoNotMintBuiltinAuthority");
    rejected_policy("BuiltinSourcePinChecked", "source_digest_mismatch", [](auto& p) { p.leaves[0].expected_source_digest.back() ^= 1; p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    rejected_policy("BuiltinDefinitionPinChecked", "definition_digest_mismatch", [](auto& p) { p.leaves[0].expected_definition_hash.back() ^= 1; p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    rejected_policy("BuiltinDependencyPinChecked", "dependency_digest_mismatch", [](auto& p) { p.leaves[0].expected_dependency_digest.back() ^= 1; p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    rejected_policy("BuiltinPolicyPinChecked", "policy_digest_mismatch", [](auto& p) { p.leaves[0].policy_digest.back() ^= 1; p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    rejected_policy("BuiltinAggregatePinChecked", "registry_digest_mismatch", [](auto& p) { p.digest.back() ^= 1; });
    rejected_policy("BuiltinOwnerGraphChecked", "uncovered_operation_owner", [](auto& p) { p.leaves[0].owners.clear(); repin(p.leaves[0]); p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    rejected_policy("BuiltinSlotCompletenessChecked", "uncovered_hook_slot", [](auto& p) { p.leaves[0].slots.pop_back(); repin(p.leaves[0]); p.digest = v2::export_registry_digest(p.leaves, installed_hook_abis()); });
    const auto foreign = root("foreign").dump();
    const auto parent_bytes = read_bytes("export.synthetic.json"), leaf_bytes = read_bytes("builtin.leaf.synthetic.json");
    const auto foreign_intake = BuiltinCatalogLoader::test_freeze_builtins({{base, parent_bytes, ""}, {leaf, leaf_bytes, ""},
        {{DefinitionNamespace::External, "foreign", 0}, foreign, ""}}, policies());
    check(!foreign_intake.snapshot && has_code(foreign_intake.diagnostics, "invalid_builtin_source"), "BuiltinTableRejectsExternalSources");
    auto mixed = Json::parse(read_bytes("builtin.leaf.synthetic.json"));
    mixed["extends"] = {{"external", "foreign"}};
    const auto mixed_bytes = mixed.dump();
    const auto closure = BuiltinCatalogLoader::test_freeze_builtins({{leaf, mixed_bytes, ""}}, policies());
    check(!closure.snapshot, "BuiltinCannotDependOnExternalIntake");
    DefinitionLimits bounds;
    bounds.diagnostics = 0;
    auto invalid = policies(); invalid.digest.back() ^= 1;
    const auto truncated = freeze(invalid, bounds);
    check(!truncated.snapshot && truncated.diagnostics.truncated && truncated.diagnostics.entries.empty(), "TruncatedBuiltinAuditIsFailure");
    bounds = {}; bounds.source_bytes = 1;
    check(!freeze(policies(), bounds).snapshot, "BuiltinSourceBudgetEnforced");
    bounds = {}; bounds.retained_logical_bytes = 1;
    check(!freeze(policies(), bounds).snapshot, "BuiltinRetentionBudgetEnforced");
    FrozenDefinitionSnapshotResult external_budget;
    bool no_throw = true;
    try { external_budget = load_v2_authority_snapshot({{{DefinitionNamespace::External, "foreign", 0}, foreign, ""}}, bounds); }
    catch (const std::exception&) { no_throw = false; }
    check(no_throw && !external_budget.snapshot && has_code(external_budget.diagnostics, "limit_exceeded"), "ExternalCompositionBudgetReturnsDiagnostic");
    return finish();
}
