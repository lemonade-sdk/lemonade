#pragma once

#include "backend_definition_authorization_internal.h"
#include "lemon/backends/configured/backend_definition_execution.h"
#include "llamacpp_external_contract.h"

#include <algorithm>
#include <functional>

namespace lemon::backends::external_fixture {
using Json = nlohmann::json;
using Check = std::function<void(bool, const std::string&)>;
inline Json fork(const std::string& name = "research-llama") {
    return Json::parse(export_fixture::read_bytes("external/" + name + ".json"));
}
inline bool code(const CompositionDiagnostics& diagnostics, const std::string& expected) {
    return std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [&](const auto& row) { return row.code == expected; });
}
inline DefinitionAuthorizationResult authorize(const std::vector<Json>& leaves, BuiltinExportContract policy = contract()) {
    const auto input = make_v2_source_snapshot(embedded_v2_sources());
    const auto exports = BuiltinCatalogLoader::test_exports({policy}, input.snapshot, installed_hook_abis(),
        v2::export_registry_digest({policy}, installed_hook_abis()));
    std::vector<std::string> bytes;
    for (const auto& leaf : leaves) bytes.push_back(leaf.dump());
    auto sources = embedded_v2_sources();
    for (std::size_t i = 0; i < leaves.size(); ++i) sources.push_back({{DefinitionNamespace::External, leaves[i].at("id"), 0}, bytes[i], ""});
    const auto frozen = BuiltinCatalogLoader::test_freeze(sources, exports);
    if (!frozen.snapshot) return {nullptr, frozen.diagnostics};
    return DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, leaves.back().at("id"), 0});
}
inline void diagnose(const CompositionDiagnostics& diagnostics) {
    for (const auto& row : diagnostics.entries) std::printf("  %s %s\n", row.code.c_str(), row.pointer.c_str());
}
inline void reject(const Check& check, const Json& leaf, const std::string& expected, const std::string& assertion) {
    const auto result = authorize({leaf});
    const bool caught = !result.definition && code(result.diagnostics, expected);
    check(caught, assertion + ":" + expected);
    if (!caught) diagnose(result.diagnostics);
}
inline Json edit(const std::string& op, const std::string& path, Json value = nullptr,
    const std::string& id = "", const std::string& at = "") {
    Json record{{"op", op}, {"path", path}};
    if (!value.is_null()) record["value"] = std::move(value);
    if (!id.empty()) record["match"] = {{"id", id}};
    if (!at.empty()) record["at"] = at;
    return record;
}
inline void export_checks(const Check& check) {
    auto policy = contract();
    const auto sources = make_v2_source_snapshot(embedded_v2_sources());
    const auto audit = [&](const BuiltinExportContract& p, std::string digest = "") {
        return v2::audit_export_contracts({p}, sources.snapshot, installed_hook_abis(),
            digest.empty() ? v2::export_registry_digest({p}, installed_hook_abis()) : digest);
    };
    check(audit(policy).entries.empty(), "ExternalContractPinsAreLiteral");
    check(policy.slots.size() == 17 && policy.owners.size() == 11, "All17BaseSlotsCovered");
    const std::vector<std::pair<std::string, std::function<void(BuiltinExportContract&)>>> attacks{
        {"source_digest_mismatch", [](auto& p) { p.expected_source_digest.back() ^= 1; }},
        {"definition_digest_mismatch", [](auto& p) { p.expected_definition_hash.back() ^= 1; }},
        {"dependency_digest_mismatch", [](auto& p) { p.expected_dependency_digest.back() ^= 1; }},
        {"policy_digest_mismatch", [](auto& p) { p.policy_digest.back() ^= 1; }},
        {"owner_graph_digest_mismatch", [](auto& p) { p.owner_graph_digest.back() ^= 1; }},
        {"hook_abi_mismatch", [](auto& p) { ++p.slots[0].hook_abi_revision; export_fixture::repin(p); }},
        {"duplicate_hook_slot", [](auto& p) { p.slots.push_back(p.slots.back()); export_fixture::repin(p); }},
        {"uncovered_hook_slot", [](auto& p) { p.slots.erase(p.slots.begin(), p.slots.begin() + 6); export_fixture::repin(p); }},
        {"fragment_envelope_escape", [](auto& p) { p.slots[0].mutable_fragments[0].maximum_records = 65; export_fixture::repin(p); }}
    };
    for (const auto& [expected, mutate] : attacks) {
        auto p = policy; mutate(p);
        const auto result = audit(p);
        check(code(result, expected), expected == "fragment_envelope_escape" ? "ExportEnvelopeRejectsWidening" : "ExternalContractAuditRejects:" + expected);
        if (!code(result, expected)) diagnose(result);
    }
    check(code(audit(policy, "sha256:0000000000000000000000000000000000000000000000000000000000000000"), "registry_digest_mismatch"),
        "ExternalContractPinsAreLiteral:registry");
}
inline void mutable_checks(const Check& check) {
    const Json identical{{"schema_version", 2}, {"id", "llamacpp"}, {"recipe", "llamacpp"},
        {"extends", {{"builtin", "llamacpp"}, {"contract", 1}}}};
    reject(check, identical, "invalid_local_executable_profile", "TrustedBaseAuditCannotAuthorizeExternalIdentity");
    const auto admitted = authorize({fork()});
    check(bool(admitted.definition), "RealLlamaScalarPresentationPreservesUnsetSentinels");
    if (!admitted.definition) { diagnose(admitted.diagnostics); return; }
    const auto& options = admitted.definition->definition().declaration.at("metadata").at("options");
    for (const auto& option : options) check(option.at("value_type") == "string" &&
        option.at("unset_values") == Json::array({nullptr, -1, "", "auto"}), "StableOptionTypesAndSentinelBytes:" + option.at("id").get<std::string>());
    for (const auto& [path, value] : std::vector<std::pair<std::string, Json>>{
        {"/install/strategy", "none"}, {"/install/install_recipe", "llamacpp"}, {"/install/pin_recipe", "research-parent"},
        {"/install/backend_fallback", "cpu"}, {"/install/repository_result", "full_params"},
        {"/launch/program/source", "managed"}, {"/launch/direct_stream_policy", "llamacpp_legacy"}}) {
        auto leaf = fork(); leaf["edits"].push_back(edit(path == "/install/backend_fallback" || path == "/install/repository_result" ? "add" : "replace", path, value));
        reject(check, leaf, path == "/install/strategy" ? "invalid_relationship" : "invalid_local_executable_profile", "LocalExecutableProfileIsClosed:" + path);
    }
    auto legacy = fork();
    legacy["edits"].push_back(edit("replace", "/launch/direct_stream_policy", "llamacpp_legacy"));
    reject(check, legacy, "invalid_local_executable_profile", "ExternalLegacyStreamRejected");
    auto basename = fork();
    basename["edits"].push_back(edit("replace", "/metadata/binary/linux", "../llama-server"));
    reject(check, basename, "invalid_local_executable_profile", "LocalExecutableBinaryIsBasename");
    auto named = fork(); named["edits"].push_back(edit("add", "/metadata/options", "--fork", "args", "/cli_flag"));
    reject(check, named, "external_named_flag", "NoExternalNamedFlags");
    auto changed = fork(); changed["edits"].push_back(edit("replace", "/metadata/options", "number", "args", "/value_type"));
    reject(check, changed, "sealed_data_changed", "StableOptionTypesCannotChange");
    changed = fork(); changed["edits"].push_back(edit("replace", "/metadata/options", Json::array({"auto"}), "args", "/unset_values"));
    reject(check, changed, "sealed_data_changed", "UnsetSentinelsCannotChange");
    changed = fork(); changed["edits"].push_back(edit("replace", "/launch/environment/set", "PATH", "", ""));
    reject(check, changed, "invalid_type", "EnvironmentCannotBecomeUnboundedObject");
    changed = fork(); changed["edits"].push_back(edit("add", "/launch/environment/set", {{"name", "PATH"}, {"value", "escape"}}));
    reject(check, changed, "mutable_domain_escape", "EnvironmentNamesAreFinite");
    changed = fork(); changed["edits"].push_back(edit("replace", "/metadata/options", 42, "args", "/help"));
    reject(check, changed, "invalid_type", "PresentationIsExactScalar");
    auto policy = contract(); policy.slots[0].mutable_inputs.erase(policy.slots[0].mutable_inputs.begin()); export_fixture::repin(policy);
    const auto restricted = authorize({fork()}, policy);
    check(!restricted.definition && code(restricted.diagnostics, "sealed_data_changed"), "MutableSurfaceIsIntersection");
}
inline Json tuple(const SealedHookGrant& grant) {
    return {{"base", grant.base_id}, {"contract", grant.contract_major}, {"source", grant.source_digest},
        {"definition", grant.definition_hash}, {"dependencies", grant.dependency_digest}, {"slot", v2::slot_json(grant.slot)},
        {"abi", grant.abi_revision}, {"parameters", grant.parameter_digest}, {"policy", grant.mutation_policy_digest}};
}
inline void authorization_checks(const Check& check) {
    const auto a = authorize({fork()});
    const auto b = authorize({fork(), fork("fork-b")});
    check(a.definition && b.definition, "ExternalForkChainAuthorizes");
    if (!a.definition || !b.definition) return;
    check(AuthorityInspection::grant_count(*a.definition) == 13 && AuthorityInspection::grant_count(*b.definition) == 10,
        "GrantTuplesOnlyAttenuate:17to13to10");
    for (std::size_t i = 0; i < AuthorityInspection::grant_count(*b.definition); ++i) {
        const auto child_tuple = tuple(AuthorityInspection::grant(*b.definition, i));
        bool found = false;
        for (std::size_t j = 0; j < AuthorityInspection::grant_count(*a.definition); ++j)
            found |= child_tuple == tuple(AuthorityInspection::grant(*a.definition, j));
        check(found && child_tuple.at("base") == "llamacpp", "GrantTuplesOnlyAttenuate:completeTuple");
    }
    auto authored = fork(); authored["merge"]["origin"] = "builtin";
    reject(check, authored, "unknown_key", "ExternalLeafNeverBuiltin");
    authored = fork(); authored["merge"]["compatibility"] = "legacy_non_chat_sse";
    reject(check, authored, "unknown_key", "ExternalLeafNeverBuiltin:compatibility");
    for (const auto* root : {"backend", "launch"}) {
        authored = fork(); authored["edits"].push_back(edit("remove", std::string("/hooks/") + root));
        reject(check, authored, std::string(root) == "launch" ? "invalid_relationship" : "mandatory_slot_removed", "MandatoryExternalRoot:" + std::string(root));
    }
    authored = fork(); authored["edits"].push_back(edit("replace", "/hooks/backend", {{"id", "llamacpp.backend.v1"}, {"params", Json::object()}}));
    reject(check, authored, "authored_hook_selection", "AuthoredHookWritesAlwaysReject");
    authored["edits"].push_back(edit("remove", "/hooks/backend"));
    reject(check, authored, "authored_hook_selection", "AuthoredHookWritesAlwaysReject:overwritten");
    authored = fork(); authored["edits"].push_back(edit("clear", "/operations", nullptr, "chat.buffered", "/request/hooks"));
    reject(check, authored, "mandatory_slot_removed", "ProtocolChainMandatoryWithOwner");
    authored = fork("fork-b");
    const auto base = DefinitionResolver(make_v2_source_snapshot(embedded_v2_sources()).snapshot).resolve({DefinitionNamespace::Builtin, "llamacpp", 1});
    for (const auto& owner : base.definition->declaration.at("operations")) if (owner.at("id") == "responses.buffered") {
        auto restored = owner; restored["request"].erase("hooks"); restored.erase("response");
        authored["edits"].push_back(edit("add", "/operations", restored));
    }
    const auto result = authorize({fork(), fork("fork-b"), Json{{"schema_version", 2}, {"id", "grandchild"}, {"recipe", "grandchild"},
        {"extends", {{"external", "fork-b"}}}, {"merge", {{"install", {{"install_recipe", "grandchild"}, {"pin_recipe", "grandchild"}}}}},
        {"edits", Json::array({authored.at("edits").back()})}}});
    check(!result.definition && code(result.diagnostics, "authored_owner_change"), "RemovedOwnerCannotReturn");
    if (result.definition || !code(result.diagnostics, "authored_owner_change")) { diagnose(result.diagnostics); }
}
}
