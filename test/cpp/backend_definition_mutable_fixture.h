#pragma once

#include "backend_definition_export_fixture.h"
#include "backend_definition_policy_internal.h"

#include <algorithm>

namespace mutable_fixture {
using namespace export_fixture;
using Kind = MutableFragmentKind;
using Type = MutableValueType;
using Phase = ReferencePhase;

inline std::vector<MutableFragmentRule> rules() {
    const auto option = [](const std::string& id) { return DefinitionTarget{"/metadata/options", RecordKey{RecordKey::Field::Id, id}, ""}; };
    std::vector<MutableFragmentRule> result = {
        {Kind::OptionPresentationV1, option("engine"), {"name", "cli_flag", "help", "group"}},
        {Kind::OptionDefaultV1, option("threads"), {"default_value", "minimum", "maximum", "enum", "unset_values"}},
        {Kind::ArgvSpellingsV1, {"/launch/argv", RecordKey{RecordKey::Field::Id, "device"}, ""}, {"flag", "aliases", "value"}},
        {Kind::ReservedFlagsV1, {"/launch/custom_args/reserved", {}, ""}},
        {Kind::CustomDefaultsV1, {"/launch/custom_args/defaults", {}, ""}},
        {Kind::DeviceSelectionV1, {"/launch/backend/fixed", {}, ""}},
        {Kind::EnvironmentEntriesV1, {"/launch/environment/set", {}, ""}},
        {Kind::SupportRowsV1, {"/metadata/support", {}, ""}},
        {Kind::InstallCasesV1, {"/install/cases", {}, ""}},
        {Kind::ContextFloorsV1, {"/launch/context_floor", {}, ""}},
        {Kind::DeviceSelectionV1, {"/metadata/default_device", {}, ""}},
        {Kind::DeviceSelectionV1, {"/metadata/rocm_channels", {}, ""}},
        {Kind::InstallCasesV1, {"/install/default_versions", {}, ""}}
    };
    for (auto& rule : result) {
        rule.maximum_length = 256;
        rule.maximum_records = 16;
        rule.maximum = 16384;
        if (rule.kind == Kind::CustomDefaultsV1 || rule.kind == Kind::EnvironmentEntriesV1 ||
            rule.kind == Kind::SupportRowsV1 || rule.kind == Kind::InstallCasesV1 || rule.kind == Kind::ContextFloorsV1)
            rule.collection = {true, true, true};
        if (rule.kind == Kind::CustomDefaultsV1) rule.domains = {{"stage", {"runtime_options", "launch"}}};
        if (rule.kind == Kind::DeviceSelectionV1) rule.domains = {{"value", {"cpu", "vulkan", "gpu", "stable", "nightly"}}};
        if (rule.kind == Kind::EnvironmentEntriesV1) rule.domains = {{"name", {"GGML_DISABLE_VULKAN", "FORK_DATA"}}};
        if (rule.kind == Kind::SupportRowsV1) rule.domains = {{"backend", {"cpu", "vulkan"}}, {"device", {"cpu", "gpu"}}, {"arch", {"x86_64", "aarch64"}}, {"channel", {"stable", "nightly"}}};
        if (rule.kind == Kind::InstallCasesV1) {
            rule.domains = {{"repo", {"test/engine", "test/fork"}}, {"backend", {"cpu", "vulkan"}}, {"arch", {"x86_64", "aarch64"}}};
            rule.references = {{"/install/version", Type::String, Phase::InstallAsset}, {"/platform/os", Type::String, Phase::Input}};
        }
    }
    return result;
}

inline BuiltinExportContract policy(bool pinned = true) {
    auto value = contract();
    value.key.id = "typed-authority";
    value.policy_revision = 4;
    value.slots.erase(std::remove_if(value.slots.begin(), value.slots.end(), [](const auto& slot) { return slot.slot.owner_kind != SlotOwnerKind::Operation; }), value.slots.end());
    value.mutable_fragments = rules();
    for (auto& slot : value.slots) slot.mutable_fragments = value.mutable_fragments;
    if (pinned) {
        const auto pins = Json::parse(read_bytes("typed-authority.synthetic.pins.json"));
        value.expected_source_digest = pins.at("source");
        value.expected_definition_hash = pins.at("definition");
        value.expected_dependency_digest = pins.at("dependencies");
        value.policy_digest = pins.at("policy");
        for (std::size_t i = 0; i < value.slots.size(); ++i) value.slots[i].mutation_policy_digest = pins.at("slots").at(i);
    }
    return value;
}

inline DefinitionAuthorizationResult authorize(const Json& child, BuiltinExportContract policy_value = policy()) {
    const auto bytes = read_bytes("typed-authority.synthetic.json");
    const auto input = make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "typed-authority", 1}, bytes, ""}});
    const auto registry = BuiltinCatalogLoader::test_exports({policy_value}, input.snapshot, installed_hook_abis(), v2::export_registry_digest({policy_value}, installed_hook_abis()));
    const auto authored = child.dump();
    const auto frozen = BuiltinCatalogLoader::test_freeze({{{DefinitionNamespace::Builtin, "typed-authority", 1}, bytes, ""},
        {{DefinitionNamespace::External, "fork", 0}, authored, ""}}, registry);
    if (!frozen.snapshot) return {nullptr, frozen.diagnostics};
    return DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, "fork", 0});
}

inline Json child() { return {{"schema_version", 2}, {"id", "fork"}, {"recipe", "fork"}, {"extends", {{"builtin", "typed-authority"}, {"contract", 1}}}}; }
inline Json edit(const std::string& path, Json value, const std::string& id = "", const std::string& at = "") {
    Json record = {{"op", "replace"}, {"path", path}, {"value", std::move(value)}};
    if (!id.empty()) { record["match"] = {{"id", id}}; if (!at.empty()) record["at"] = at; }
    auto result = child(); result["edits"] = Json::array({record}); return result;
}
}
