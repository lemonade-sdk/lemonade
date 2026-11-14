#include "backend_definition_v2_internal.h"
#include "lemon/backends/configured/backend_definition.h"

#include <cstdio>
#include <fstream>
#include <type_traits>

using namespace lemon::backends;
using Json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
Json read(const std::string& name) {
    std::ifstream stream(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/" + name);
    return Json::parse(stream);
}
DefinitionKey key(const Json& value, DefinitionNamespace space = DefinitionNamespace::Builtin) {
    return {space, value["id"], space == DefinitionNamespace::Builtin ? 1u : 0u};
}
DefinitionSnapshotResult snapshot(const std::vector<Json>& sources, DefinitionNamespace space = DefinitionNamespace::Builtin) {
    std::vector<std::string> storage;
    for (const auto& source : sources) storage.push_back(source.dump());
    std::vector<DefinitionSourceView> views;
    for (std::size_t i = 0; i < sources.size(); ++i) views.push_back({key(sources[i], space), storage[i], "/private/secret.json"});
    return make_v2_source_snapshot(views);
}
DefinitionResolutionResult resolve(const std::vector<Json>& sources, const DefinitionKey& leaf, const DefinitionLimits& limits = {}) {
    DefinitionResolver resolver(snapshot(sources, leaf.space).snapshot, limits);
    return resolver.resolve(leaf);
}
Json child(const std::string& id = "fork", const std::string& parent = "llamacpp") {
    return {{"schema_version", 2}, {"id", id}, {"recipe", id}, {"extends", {{"builtin", parent}, {"contract", 1}}}};
}
bool code(const DefinitionResolutionResult& result, const std::string& expected) {
    return !result.definition && !result.diagnostics.entries.empty() && result.diagnostics.entries.front().code == expected;
}
void algebra() {
    for (const auto& fixture : read("algebra.json")) {
        DefinitionLimits limits;
        std::size_t retained = 0;
        v2::Budget budget{limits, {}, retained};
        v2::LayerState state;
        const DefinitionKey document{DefinitionNamespace::Builtin, "fixture", 1};
        v2::apply_layer(state, Json{{"merge", fixture["input"]}}, document, budget);
        const auto original = state.root;
        const auto saved = v2::materialize(original, budget);
        bool present = false;
        try {
            v2::apply_layer(state, fixture, document, budget);
            present = true;
            check(fixture.contains("expected") && v2::materialize(state.root, budget) == fixture["expected"], fixture["name"]);
        } catch (const v2::EditFailure& failure) {
            check(fixture.contains("expected_error") && failure.code == fixture["expected_error"]["code"] && failure.pointer == fixture["expected_error"]["pointer"], fixture["name"]);
            if (fixture["expected_error"].contains("related_pointer")) {
                const auto removed = v2::removed_at(state.removals, failure.target);
                check(removed && removed->pointer == fixture["expected_error"]["related_pointer"], "contradictory pair retains deletion source");
            }
        }
        check(present == fixture.contains("expected"), "rejection has no result");
        check(v2::materialize(original, budget) == saved, "original input remains immutable");
    }
}
}
int main() {
    static_assert(!std::is_convertible_v<std::shared_ptr<const ResolvedDefinition>, std::shared_ptr<const BackendDefinition>>);
    algebra();
    const auto base = read("llamacpp.example.json"), hrx = read("hrx.example.json");
    const auto base_result = resolve({base}, key(base));
    check(base_result.definition != nullptr, "base resolves without execution or hook lookup");
    const auto hrx_result = resolve({hrx, base}, key(hrx));
    check(hrx_result.definition != nullptr, "forward builtin HRX delta resolves");
    if (!base_result.definition || !hrx_result.definition) {
        for (const auto& result : {base_result, hrx_result}) for (const auto& error : result.diagnostics.entries) std::printf("%s %s %s\n", error.definition.c_str(), error.code.c_str(), error.pointer.c_str());
        return 1;
    }
    const auto& output = hrx_result.definition->declaration;
    check(output["operations"].size() == 11 && output["launch"]["argv"].size() == 6, "HRX retains protocol and surviving flags");
    check(!output["launch"].contains("context_floor") && output["hooks"].empty() && !output["artifacts"].contains("draft"), "HRX removes vanilla policy");
    check(output["launch"]["environment"]["set"] == Json::array({{{"name", "GGML_DISABLE_VULKAN"}, {"value", "1"}}}), "replace container then add inside it succeeds");
    check(output["metadata"]["options"][0]["id"] == "args" && output["metadata"]["options"][0]["name"] == "hrx_args", "public option rename preserves stable identity");
    check(hrx_result.definition->dependencies.size() == 1 && hrx_result.definition->dependencies[0].document.id == "llamacpp", "root-to-parent identities");
    for (const auto& fixture : read("graphs.json")) {
        std::vector<Json> sources;
        const bool cyclic = fixture.contains("cycle");
        const auto count = cyclic ? fixture["cycle"].get<int>() : fixture["parents"].get<int>() + 1;
        const auto space = fixture.value("space", "builtin") == "builtin" ? DefinitionNamespace::Builtin : DefinitionNamespace::External;
        for (int i = 0; i < count; ++i) {
            auto value = i == 0 && !cyclic ? base : child("node" + std::to_string(i), "node" + std::to_string(cyclic ? (i + 1) % count : i - 1));
            value["id"] = value["recipe"] = "node" + std::to_string(i);
            if (space == DefinitionNamespace::External && value.contains("extends")) value["extends"] = {{"external", "node" + std::to_string(i - 1)}};
            sources.push_back(value);
        }
        const auto result = resolve(sources, key(sources.back(), space));
        check(fixture.value("valid", false) ? result.definition != nullptr : code(result, fixture["code"]), fixture["name"]);
        if (cyclic) {
            const auto& cycle = result.diagnostics.entries[0].related;
            check(cycle.size() == static_cast<std::size_t>(count + 1), "complete cycle chain");
            bool exact = cycle.size() == static_cast<std::size_t>(count + 1);
            for (std::size_t i = 0; i < cycle.size(); ++i) exact &= cycle[i].document.id == "node" + std::to_string((count - 1 + static_cast<int>(i)) % count) && cycle[i].pointer == "/extends";
            check(exact,"exact cycle sequence from leaf identity");
        }
    }
    auto fork = child();
    check(code(resolve({fork}, key(fork)), "missing_parent"), "missing parent");
    fork["extends"]["contract"] = 2;
    check(code(resolve({base, fork}, key(fork)), "contract_mismatch"), "contract mismatch");
    fork["extends"] = {{"external", "llamacpp"}};
    check(code(resolve({base, fork}, key(fork)), "missing_parent"), "namespace is part of identity");
    fork = child();
    fork["extends"]["expected_digest"] = "sha256:" + std::string(64, '0');
    check(code(resolve({base, fork}, key(fork)), "expected_digest_mismatch"), "expected digest compares materialized parent");
    fork["extends"]["expected_digest"] = base_result.definition->definition_hash;
    check(resolve({fork, base}, key(fork)).definition != nullptr, "expected materialized hash accepted");
    fork = child();
    auto source_snapshot = snapshot({fork, base});
    DefinitionResolver resolver(source_snapshot.snapshot);
    const auto parent = resolver.resolve(key(base));
    auto invalid = child("invalid");
    invalid["edits"] = Json::array({{{"op", "remove"}, {"path", "/metadata/display_name"}}});
    DefinitionResolver isolated(snapshot({base, fork, invalid}).snapshot);
    const auto before = isolated.resolve(key(base));
    check(!isolated.resolve(key(invalid)).definition, "invalid child rejected");
    check(isolated.resolve(key(base)).definition->declaration == before.definition->declaration, "failed child cannot corrupt cached parent");
    check(resolver.resolve(key(fork)).definition->declaration == resolve({base, fork}, key(fork)).definition->declaration, "memoization and discovery order do not change output");
    auto changed = base;
    changed["merge"]["metadata"]["display_name"] = "changed";
    check(resolve({changed, fork}, key(fork)).definition->definition_hash != resolve({base, fork}, key(fork)).definition->definition_hash, "inherited change affects effective hash");
    fork["merge"]["metadata"]["display_name"] = "override";
    const auto a = resolve({base, fork}, key(fork)).definition, b = resolve({changed, fork}, key(fork)).definition;
    check(a->definition_hash == b->definition_hash && a->dependency_digest != b->dependency_digest, "overridden parent change retains effective hash but changes dependency identity");
    check(parent.definition->declaration == base_result.definition->declaration, "earlier result remains unchanged");
    const auto hash_fixtures = read("hashes.json");
    check(base_result.definition->source_digest == hash_fixtures["expected_base"]["source_digest"] && base_result.definition->definition_hash == hash_fixtures["expected_base"]["definition_hash"] && base_result.definition->dependency_digest == hash_fixtures["expected_base"]["dependency_digest"],"independent domain-separated hashes");
    for (const auto* field : {"same", "different"}) for (const auto& pair : hash_fixtures[field]) {
        auto left = base, right = base;
        left["merge"]["metadata"]["config_extra"]["number"] = Json::parse(pair[0].get<std::string>());
        right["merge"]["metadata"]["config_extra"]["number"] = Json::parse(pair[1].get<std::string>());
        check((resolve({left}, key(left)).definition->source_digest == resolve({right}, key(right)).definition->source_digest) == (std::string(field) == "same"), "canonical numeric/unicode hash fixture " + pair.dump());
    }
    fork = child();
    fork["edits"] = Json::array({{{"op", "replace"}, {"path", "/metadata/display_name"}, {"value", "Llama.cpp GPU"}}, {{"op", "replace"}, {"path", "/metadata/display_name"}, {"value", "Llama.cpp GPU"}}});
    const auto history = resolve({base, fork}, key(fork));
    const auto plain = resolve({base, child()}, key(child()));
    check(history.definition->definition_hash == plain.definition->definition_hash && history.definition->source_digest != plain.definition->source_digest, "equal output histories preserve hash and differ in source identity");
    for (const auto& edit : std::vector<Json>{
        {{"op","replace"},{"path","/launch/argv"},{"match",{{"id","ctx"}}},{"at","/id"},{"value","renamed"}},
        {{"op","replace"},{"path","/launch/argv"},{"match",{{"id","ctx"}}},{"value",{{"id","renamed"},{"args",Json::array()}}}},
        {{"op","clear"},{"path","/launch/argv"},{"match",{{"id","ctx"}}}},
        {{"op","remove"},{"path","/id"}}}) {
        fork = child(); fork["edits"] = Json::array({edit});
        check(code(resolve({base,fork},key(fork)),"identity_protected"),"protect record and envelope identity");
    }
    for (const auto& edit : std::vector<Json>{
        {{"op","replace"},{"path","/launch/argv"},{"match",{{"name","ctx"}}},{"value",{{"id","ctx"},{"args",Json::array()}}}},
        {{"op","add"},{"path","/launch/argv/0/args"},{"value","x"}},
        {{"op","replace"},{"path","/launch/argv/-"},{"value",Json::object()}}}) {
        fork = child(); fork["edits"] = Json::array({edit});
        check(!resolve({base,fork},key(fork)).definition,"reject selector kind and array indexing");
    }
    fork = child();
    fork["merge"]["metadata"]["config_extra"] = {{"0", {{"", {{"a/b~c", 1}}}}}};
    fork["edits"] = Json::array({{{"op","replace"},{"path","/metadata/config_extra/0//a~1b~0c"},{"value",nullptr}}});
    const auto escaped = resolve({base,fork},key(fork));
    check(escaped.definition && escaped.definition->declaration["metadata"]["config_extra"]["0"][""]["a/b~c"].is_null(), "numeric object keys, empty names and pointer escapes remain legal");
    fork = child();
    fork["merge"]["operations"] = base["merge"]["operations"];
    fork["merge"]["operations"][0]["request"]["set"] = {{"option","ordinary data"},{"payload",{{"literal",{{"option","not-a-reference"}}}}}};
    check(resolve({base,fork},key(fork)).definition != nullptr,"arbitrary request member names and literal contents are not option references");
    fork["merge"]["operations"][0]["request"]["set"]["payload"] = {{"option","missing"}};
    check(code(resolve({base,fork},key(fork)),"invalid_reference"),"schema-located option expressions require declared ID");
    changed = base; changed["schema_version"] = 1;
    check(!snapshot({changed}).snapshot,"v1 parent excluded from v2 snapshot");
    const auto pretty = base.dump(4), compact = base.dump();
    DefinitionResolver pretty_resolver(make_v2_source_snapshot({{key(base),pretty,""}}).snapshot);
    DefinitionResolver compact_resolver(make_v2_source_snapshot({{key(base),compact,""}}).snapshot);
    check(pretty_resolver.resolve(key(base)).definition->source_digest == compact_resolver.resolve(key(base)).definition->source_digest,"formatting does not change hashes");
    std::printf("%d/%d checks passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}
