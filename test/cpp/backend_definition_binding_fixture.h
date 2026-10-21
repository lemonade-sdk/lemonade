#pragma once

#include "backend_definition_binding_internal.h"
#include "catalog_admission_fixture.h"

#include <map>
#include <stdexcept>

namespace binding_fixture {
using namespace admission_fixture;
namespace ex = lemon::backends::execution;
namespace bd = lemon::backends::binding;
namespace typed = lemon::backends::typed;
inline std::map<std::string, unsigned> calls;
inline bool allow_entries = false;
inline unsigned service_calls = 0;

inline void zero(const std::string& path) {
    check(calls.empty() && service_calls == 0, "HookBearingCompileAndBindInvokeNothing:" + path);
}
inline void diagnostics(const CompositionDiagnostics& value) {
    for (const auto& error : value.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
}
inline Json document(bool hrx = false) {
    auto value = Json::parse(read_bytes("binding.synthetic.json"));
    if (hrx) {
        value["id"] = value["recipe"] = "binding-hrx";
        value["merge"].erase("hooks");
        value["merge"]["launch"]["argv"].erase(1);
        value["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
        for (auto& op : value["merge"]["operations"]) {
            op["request"].erase("hooks");
            op.erase("response");
        }
    }
    return value;
}
inline std::shared_ptr<const FrozenBuiltinCatalog> builtins(bool hrx = false, const Json* replacement = nullptr) {
    const auto doc = replacement ? *replacement : document(hrx);
    const auto name = doc.at("id").get<std::string>();
    const DefinitionKey key{DefinitionNamespace::Builtin, name, 1};
    const auto bytes = doc.dump();
    const std::vector<DefinitionSourceView> sources{{key, bytes, ""}};
    const auto snapshot = make_v2_source_snapshot(sources);
    const auto resolved = DefinitionResolver(snapshot.snapshot).resolve(key);
    if (!resolved.definition) { diagnostics(snapshot.diagnostics); diagnostics(resolved.diagnostics); throw std::logic_error("Fixture resolution failed"); }
    const auto& source = *resolved.definition;
    BuiltinExportContract policy{{name, 1}, source.source_digest, source.definition_hash, source.dependency_digest, 1, "", {}};
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    for (const auto& selection : v2::hook_selections(source.declaration, budget)) {
        ExportSlotPolicy slot{selection.slot, 1, v2::parameter_digest(selection.parameters), "", SlotRemoval::Independent, {}};
        slot.owner_semantics_digest = v2::owner_semantics_digest(selection.owner_semantics);
        slot.ordered_hooks = selection.ordered_hooks;
        slot.effects = {HookEffect::Pure};
        policy.slots.push_back(std::move(slot));
    }
    for (const auto& op : source.declaration.at("operations")) {
        OperationOwnerPolicy owner;
        owner.id = op.at("id");
        owner.semantics = v2::operation_owner_semantics(op, budget);
        owner.request_hooks = v2::operation_hook_chain(op, "request", budget);
        owner.response_hooks = v2::operation_hook_chain(op, "response", budget);
        owner.handler_hooks = v2::operation_hook_chain(op, "handler", budget);
        owner.sealed_fields = v2::operation_owner_fields();
        for (const auto& sibling : source.declaration.at("operations"))
            if (sibling.at("id") != owner.id && v2::operation_owners_collide(op, sibling, budget)) owner.collisions.push_back(sibling.at("id"));
        policy.owners.push_back(std::move(owner));
    }
    repin(policy);
    BuiltinExecutionPolicies policies{{policy}, v2::export_registry_digest({policy}, installed_hook_abis())};
    const auto frozen = BuiltinCatalogLoader::test_freeze_builtins(sources, policies);
    if (!frozen.snapshot) { diagnostics(frozen.diagnostics); throw std::logic_error("Fixture policy failed"); }
    return CatalogAdmissionBuilder::test_builtins(frozen.snapshot, sources);
}
struct Fixture {
    std::shared_ptr<const AdmittedCatalog> admitted;
    std::shared_ptr<const AuthorizedDefinition> token;
    std::shared_ptr<const CompiledDefinition> compiled;
};
inline Fixture fixture(bool hrx = false) {
    const auto result = admit({}, builtins(hrx));
    if (!result.catalog) { diagnostics(result.diagnostics); throw std::logic_error("Fixture admission failed"); }
    auto token = result.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, hrx ? "binding-hrx" : "binding-fixture", 1});
    const auto compiled = compile_execution_definition(token);
    if (!compiled.definition) { diagnostics(compiled.diagnostics); throw std::logic_error("Hook fixture compilation failed"); }
    return {result.catalog, token, compiled.definition};
}
inline ExecutionBindResult bind(const Fixture& value, const ExecutionBindLimits& limits = {}) {
    return bind_execution_definition(value.admitted, value.compiled, limits);
}
inline void reject(const Fixture& value, const std::string& code, const std::string& label) {
    const auto result = bind(value);
    check(!result.definition && has_code(result.diagnostics, code), label);
    if (result.definition || !has_code(result.diagnostics, code)) diagnostics(result.diagnostics);
    zero(label);
}
inline Fixture changed(const Fixture& value, const std::function<void(ex::Program&)>& change) {
    auto program = ex::Inspection::program(*value.compiled);
    change(program);
    return {value.admitted, value.token, ex::Inspection::test_program(*value.compiled, std::move(program))};
}

class ConfinedReads final : public typed::ArtifactReadServices {
public:
    unsigned reads = 0;
    typed::ArtifactHandle root() const { return handle(0); }
    typed::ArtifactHandle file() const { return handle(1); }
    std::optional<typed::ArtifactHeader> header(typed::ArtifactHandle value) override {
        if (index(value) != 1) throw std::logic_error("foreign file");
        ++reads;
        return typed::ArtifactHeader{"qwen3", 32768, {true, true, true}};
    }
    std::string prefix(typed::ArtifactHandle value, std::size_t bytes) override {
        if (index(value) != 1 || bytes != 4) throw std::logic_error("foreign read");
        ++reads;
        return "GGUF";
    }
    std::vector<typed::ArtifactEntry> entries(typed::ArtifactHandle value) override {
        if (index(value) != 0) throw std::logic_error("foreign root");
        ++reads;
        return {{file(), "snapshots/active/model-Q4_K_M.gguf", "cache/model-Q4_K_M.gguf", true, true}};
    }
};
class EffectSentinels final : public typed::BackendSelectionServices, public typed::InstallFactsServices,
    public typed::EnvironmentServices, public typed::SystemProbeServices {
    [[noreturn]] void fail() { ++service_calls; throw std::logic_error("unexpected effect"); }
public:
    bool supports(const std::string&) override { fail(); }
    typed::DeviceCheck validate_device(const typed::DeviceValidationInput&) override { fail(); }
    std::string therock_version() override { fail(); }
    std::string rocm_asset_family() override { fail(); }
    std::string cuda_architecture() override { fail(); }
    std::optional<std::string> parent(typed::ParentKey) override { fail(); }
    std::string runtime(typed::RuntimeQuery) override { fail(); }
    std::string llama_version() override { fail(); }
    bool kfd_present() override { fail(); }
    bool hip_plugin_present() override { fail(); }
};
}

#ifndef LEMONADE_RUNTIME_TEST
void lemon::backends::typed::observe_entry(const char* entry) {
    ++binding_fixture::calls[entry];
    if (!binding_fixture::allow_entries) throw std::logic_error("HookBearingCompileAndBindInvokeNothing");
}
#endif
