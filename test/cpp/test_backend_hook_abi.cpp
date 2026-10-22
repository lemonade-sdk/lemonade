#include "backend_typed_hook_registry_internal.h"
#include "lemon/backends/configured/backend_hook_abi.h"
#include "lemon/backends/configured/backend_hook_registry.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <type_traits>

using namespace lemon::backends;
using json = nlohmann::json;
namespace {
int checks = 0, failures = 0, invocations = 0;
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
const BackendOps* artifacts() { ++invocations; return nullptr; }
json request(json value) { ++invocations; return value; }
json response(json value, const json&, const std::string&) { ++invocations; return value; }
json launch(const LaunchHookContext&) { ++invocations; return json::object(); }
json operation(const OperationHookContext&, const json&) { ++invocations; return json::object(); }
std::string start_failure(const std::string&, const std::vector<std::string>&) { ++invocations; return ""; }
void sink_operation(const OperationHookContext&, const json&, httplib::DataSink&) { ++invocations; }

namespace typed = lemon::backends::typed;
std::map<std::string, unsigned> entries_called;

void typed_assertions() {
    const auto& entries = typed::installed_targets();
    typed::audit_targets(entries);
    check(entries.size() == 10, "ExactTenTypedTargets");
    json actual = json::array();
    for (const auto& entry : entries) {
        std::vector<int> effects, requires, provides;
        for (const auto value : entry.effects) effects.push_back(static_cast<int>(value));
        for (const auto value : entry.requires) requires.push_back(static_cast<int>(value));
        for (const auto value : entry.provides) provides.push_back(static_cast<int>(value));
        actual.push_back({{"contract", static_cast<int>(entry.contract)}, {"id", entry.id}, {"stage", entry.stage},
            {"abi", entry.abi_revision}, {"implementation", entry.implementation_revision},
            {"effects", effects}, {"requires", requires}, {"provides", provides}});
        const auto* abi = find_hook_abi(entry.id);
        check(abi && abi->stage == entry.stage && abi->revision == entry.abi_revision &&
            abi->implementation_revision == entry.implementation_revision, "typed target agrees with immutable ABI projection");
    }
    std::ifstream fixture(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/hook-binding.inventory.json");
    json expected;
    fixture >> expected;
    check(actual == expected.at("targets"), "TypedEffectsAndDependenciesPinned: independent literal inventory");
    const auto rejected = [&](const auto& change, const std::string& assertion) {
        auto copy = entries;
        change(copy);
        bool failed = false;
        try { typed::audit_targets(copy); }
        catch (const std::logic_error& error) { failed = std::string(error.what()).find(assertion) != std::string::npos; }
        check(failed, assertion.c_str());
    };
    rejected([](auto& copy) { copy.pop_back(); }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy.push_back(copy.front()); }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[1] = copy[0]; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[0].contract = static_cast<typed::Contract>(999); }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[0].id = "arbitrary.target"; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[6].stage = "prepare"; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[5].target = typed::RequestTransform(&typed::llamacpp::tool_schema_limits); }, "ExactTenTypedTargets");
    rejected([](auto& copy) { copy[9].target = typed::RequestTransform(&typed::llamacpp::max_tokens_alias); }, "ExactTenTypedTargets");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        rejected([&](auto& copy) { copy[i].abi_revision = 2; }, "TypedEffectsAndDependenciesPinned");
        rejected([&](auto& copy) { copy[i].implementation_revision = 2; }, "TypedEffectsAndDependenciesPinned");
        rejected([&](auto& copy) { copy[i].effects.push_back(typed::Effect::SystemProbe); }, "TypedEffectsAndDependenciesPinned");
        rejected([&](auto& copy) { copy[i].requires.clear(); }, "TypedEffectsAndDependenciesPinned");
        rejected([&](auto& copy) { copy[i].provides.clear(); }, "TypedEffectsAndDependenciesPinned");
        rejected([&](auto& copy) {
            std::visit([](auto& target) {
                using T = std::decay_t<decltype(target)>;
                if constexpr (std::is_pointer_v<T>) target = nullptr;
                else if constexpr (std::is_same_v<T, typed::BackendSelectionOps>) target.select_backend = nullptr;
                else if constexpr (std::is_same_v<T, typed::SystemOps>) target.resolve_version = nullptr;
                else target.metadata = nullptr;
            }, copy[i].target);
        }, "ExactTenTypedTargets");
    }
    rejected([](auto& copy) { std::get<typed::BackendSelectionOps>(copy[1].target).validate_device = nullptr; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { std::get<typed::SystemOps>(copy[8].target).check_install = nullptr; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { std::get<typed::ArtifactOps>(copy[0].target).validate_registration = nullptr; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { std::get<typed::ArtifactOps>(copy[0].target).validate_file = nullptr; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { std::get<typed::ArtifactOps>(copy[0].target).resolve_path = nullptr; }, "ExactTenTypedTargets");
    rejected([](auto& copy) { std::get<typed::ArtifactOps>(copy[0].target).find_imported = nullptr; }, "ExactTenTypedTargets");
    check(entries_called.empty(), "TypedAuditInvokesNothing: successful and rejected audits");
}

class Services final : public typed::BackendSelectionServices, public typed::InstallFactsServices,
    public typed::EnvironmentServices, public typed::SystemProbeServices {
public:
    unsigned calls = 0;
    bool supports(const std::string& backend) override { ++calls; return backend == "rocm" || backend == "cpu"; }
    typed::DeviceCheck validate_device(const typed::DeviceValidationInput&) override { ++calls; return {true, ""}; }
    std::string therock_version() override { ++calls; return "v7.12.1"; }
    std::string rocm_asset_family() override { ++calls; return "gfx1151"; }
    std::string cuda_architecture() override { ++calls; return "sm_120"; }
    std::optional<std::string> parent(typed::ParentKey) override { ++calls; return std::nullopt; }
    std::string runtime(typed::RuntimeQuery) override { ++calls; return "fake"; }
    std::string llama_version() override { ++calls; return "b1234"; }
    bool kfd_present() override { ++calls; return true; }
    bool hip_plugin_present() override { ++calls; return false; }
    typed::StagingCapability staging() const { return staging_capability(typed::StagingScope::ManagedTheRock); }
};

void check_other_adapters() {
    namespace impl = typed::llamacpp;
    Services services;
    const auto selected = impl::select_backend({"rocm", "nightly", {}}, services);
    check(selected.backend == "rocm-nightly" && selected.use_gpu, "typed backend resolves channel");
    check(impl::select_backend({"", "stable", {"missing", "cpu"}}, services).backend == "cpu", "typed fallback uses approved support query");
    const auto before = services.calls;
    check(impl::validate_device({"cpu", ""}, services).supported && services.calls == before, "empty device does not probe");
    check(impl::validate_device({"rocm-stable", "ROCm0"}, services).supported, "separate device validation entry");
    typed::InstallFactsInput install;
    check(!impl::install_context(install, services).therock_version && services.calls == before + 1, "repository phase obtains no asset facts");
    install.phase = typed::InstallPhase::Asset;
    install.backend = "rocm-stable";
    install.requested = {typed::InstallFact::TheRockVersion};
    const auto facts = impl::install_context(install, services);
    check(facts.therock_version == "7.12" && !facts.cuda_architecture && !facts.rocm_asset_family, "only requested install facts");
    install.phase = typed::InstallPhase::Repository;
    bool rejected = false;
    try { impl::install_context(install, services); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "install phase mismatch fails closed");
    const auto defaults = impl::runtime_options({{"dflash", "mtp"}, "org/DFlash-draft.gguf"});
    check(defaults.dflash && !defaults.mtp, "pure runtime facts retain DFlash precedence");
    typed::PrepareInput input;
    input.model_type = lemon::ModelType::EMBEDDING;
    input.context_size = 100;
    input.mmproj_path = "mmproj.gguf";
    const auto prepared = impl::prepare(input);
    check(prepared.embedding && prepared.context_size == 100 && prepared.mmproj_on_cpu, "prepare returns facts; data retains context floor");
    const auto lazy_before = services.calls;
    check(impl::environment({}, services).assignments.empty() && services.calls == lazy_before, "irrelevant environment does not probe");
    static_assert(!std::is_default_constructible_v<typed::StagingCapability>);
    static_assert(!std::is_constructible_v<typed::StagingCapability, std::string>);
    Services other;
    typed::EnvironmentInput environment;
    environment.staging = other.staging();
    rejected = false;
    try { impl::environment(environment, services); } catch (const std::logic_error&) { rejected = true; }
    check(rejected && services.calls == lazy_before, "foreign staging capability fails before runtime queries");
    check(impl::resolve_version({"cpu", "b99"}, services) == "b99" && services.calls == lazy_before, "managed version does not probe");
    check(impl::resolve_version({"system", ""}, services) == "b1234", "system version uses restricted probe");
    check(!impl::check_install({typed::Platform::Linux, "system", true}, services).installed, "system install checks typed HIP plugin probe");
    check(impl::max_tokens_alias({{"max_completion_tokens", 7}}).at("max_tokens") == 7, "request alias uses existing transform");
    const auto sanitized = impl::tool_schema_limits({{"tools", {{{"parameters", {{"maxLength", 2000}}}}}}});
    check(!sanitized.at("tools")[0].at("parameters").contains("maxLength"), "schema target uses existing transform");
    check(impl::client_model({{"model", "backend"}}, {{"model", "client"}}, "fallback").at("model") == "client", "response target uses existing transform");
    const std::array<const char*, 11> observed = {"backend.select_backend", "backend.validate_device", "install_context",
        "runtime_options", "prepare", "environment", "system.resolve_version", "system.check_install",
        "max_tokens_alias", "tool_schema_limits", "client_model"};
    for (const auto* entry : observed) check(entries_called[entry] > 0, "TypedAuditInvokesNothing: instrumentation observes actual entry");
}
}

void lemon::backends::typed::observe_entry(const char* entry) { ++entries_called[entry]; }

int main() {
    const auto& installed = installed_backend_hooks();
    audit_hook_abi_metadata(installed);
    auto registrations = installed.registrations();
    const std::vector<HookTarget> targets = {ArtifactHook(&artifacts), RequestHook(&request), ResponseHook(&response),
        LaunchHook(&launch), OperationHook(&operation), StartFailureHook(&start_failure), SinkOperationHook(&sink_operation)};
    for (auto& registration : registrations) registration.target = targets.at(static_cast<std::size_t>(registration.stage));
    audit_hook_abi_metadata(BackendHookRegistry(registrations));
    check(invocations == 0, "ABI audit never invokes executable targets");
    check(registrations.size() == 12, "all installed executable hooks described");
    check(installed_legacy_hook_abis().size() == 12 && installed_hook_abis().size() == 19,
        "LegacyAndTypedInventoriesAreSeparate");
    const auto rejected = [&](const std::function<void(std::vector<HookRegistration>&)>& change) {
        auto copy = registrations;
        change(copy);
        try { audit_hook_abi_metadata(BackendHookRegistry(std::move(copy))); }
        catch (const std::logic_error&) { return true; }
        return false;
    };
    check(rejected([](auto& entries) { entries.pop_back(); }), "missing executable registration rejected");
    check(rejected([](auto& entries) { entries[0].id = "unknown.executable"; }), "unknown executable registration rejected");
    check(rejected([](auto& entries) { entries[0].id = "llamacpp.artifacts.v1"; }), "TypedArtifactCannotImpersonateLegacyProvider");
    check(rejected([&](auto& entries) { entries[0].stage = HookStage::Request; entries[0].target = targets[1]; }), "executable stage change rejected");
    check(invocations == 0, "rejected audits never invoke executable targets");
    typed_assertions();
    check_other_adapters();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
