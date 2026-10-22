#pragma once

#include "backend_catalog_admission_internal.h"
#include "lemon/backends/configured/backend_definition_binding.h"
#include "llamacpp_external_fixture.h"

namespace lemon::backends::external_fixture {
inline std::shared_ptr<const FrozenBuiltinCatalog> builtins(BuiltinExportContract policy = contract()) {
    const auto sources = make_v2_source_snapshot(embedded_v2_sources());
    const auto registry = BuiltinCatalogLoader::test_exports({policy}, sources.snapshot, installed_hook_abis(),
        v2::export_registry_digest({policy}, installed_hook_abis()));
    const auto authority = BuiltinCatalogLoader::test_freeze(embedded_v2_sources(), registry);
    return CatalogAdmissionBuilder::test_builtins(authority.snapshot, embedded_v2_sources());
}
inline CatalogAdmissionResult admit(const std::vector<Json>& leaves, std::shared_ptr<const FrozenBuiltinCatalog> builtin = builtins(),
    const DefinitionLimits& limits = {}) {
    std::vector<std::string> bytes;
    for (const auto& leaf : leaves) bytes.push_back(leaf.dump());
    std::vector<ExternalDefinitionInput> sources;
    for (const auto& value : bytes) sources.push_back({value, ""});
    const auto snapshot = make_catalog_source_snapshot(std::move(builtin), sources, limits);
    return snapshot.snapshot ? admit_catalog(snapshot.snapshot) : CatalogAdmissionResult{nullptr, snapshot.diagnostics};
}
inline std::shared_ptr<const ExecutableDefinition> bound(const Json& leaf = fork()) {
    const auto admitted = admit({leaf});
    if (!admitted.catalog) { diagnose(admitted.diagnostics); throw std::logic_error("External fixture admission failed"); }
    const auto compiled = compile_execution_definition(admitted.catalog->v2_definition(leaf.at("id")));
    if (!compiled.definition) { diagnose(compiled.diagnostics); throw std::logic_error("External fixture compilation failed"); }
    const auto result = bind_execution_definition(admitted.catalog, compiled.definition);
    if (!result.definition) { diagnose(result.diagnostics); throw std::logic_error("External fixture binding failed"); }
    return result.definition;
}
inline void admission_checks(const Check& check) {
    const auto accepted = admit({fork(), fork("fork-b")});
    check(bool(accepted.catalog), "RealExportDerivedChainCatalogAdmission");
    for (const auto& [pointer, value, expected] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"/metadata/config_section", "research-llama", "section_collision"},
        {"/metadata/options", "research_llama_args", "option_collision"}}) {
        auto leaf = fork("fork-b");
        leaf["edits"].push_back(edit("replace", pointer, value, pointer == "/metadata/options" ? "args" : "", pointer == "/metadata/options" ? "/name" : ""));
        const auto result = admit({fork(), leaf});
        check(!result.catalog && code(result.diagnostics, expected), "EveryLeafOwnsItsNamesPathsAndPins:" + expected);
    }
    auto changed = contract(); ++changed.policy_revision; export_fixture::repin(changed);
    const auto second = admit({fork(), fork("fork-b")}, builtins(changed));
    check(accepted.catalog && second.catalog && accepted.catalog->admission_id() != second.catalog->admission_id(), "ExternalPolicyInvalidatesAdmissionIdentity");
    DefinitionLimits limits; limits.chain_documents = 1;
    const auto bounded = admit({fork(), fork("fork-b")}, builtins(), limits);
    check(!bounded.catalog && code(bounded.diagnostics, "limit_exceeded"), "ExternalGraphBoundsFailClosed");
}
inline void owner_checks(const Check& check) {
    for (const auto& owner : contract().owners) {
        auto leaf = fork(); leaf["edits"].push_back(edit("remove", "/operations", nullptr, owner.id));
        const auto removed = authorize({leaf});
        check(bool(removed.definition), "WholeExternalOwnerRemovable:" + owner.id);
        if (!removed.definition) { diagnose(removed.diagnostics); continue; }
        auto restored = owner.semantics;
        restored["id"] = "laundered-owner";
        restored["request"] = {{"base", "identity"}};
        auto child = Json{{"schema_version", 2}, {"id", "descendant"}, {"recipe", "descendant"},
            {"extends", {{"external", "research-llama"}}},
            {"merge", {{"install", {{"install_recipe", "descendant"}, {"pin_recipe", "descendant"}}}}},
            {"edits", Json::array({edit("add", "/operations", restored)})}};
        const auto rejected = authorize({leaf, child});
        check(!rejected.definition && code(rejected.diagnostics, "protected_owner_recreated"), "RemovedOwnerCannotReturnUnderAnotherId:" + owner.id);
        if (rejected.definition || !code(rejected.diagnostics, "protected_owner_recreated")) diagnose(rejected.diagnostics);
    }
}
}
