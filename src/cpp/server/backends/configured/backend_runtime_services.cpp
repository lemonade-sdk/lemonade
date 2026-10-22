#include "backend_runtime_services_internal.h"
#include "lemon/backends/configured/backend_definition_binding.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <regex>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace lemon::backends::runtime {
namespace {
std::atomic<std::uint64_t> next_nonce{1};
std::uint64_t invocation_nonce() {
    auto nonce = next_nonce.load();
    do {
        if (nonce == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Invocation nonce exhausted");
    } while (!next_nonce.compare_exchange_weak(nonce, nonce + 1));
    return nonce;
}
}

Frame::Frame(const ExecutableDefinition& owner) : executable(&owner), nonce(invocation_nonce()) {}
void check_cancelled(const utils::RequestCancelToken& cancel) {
    if (cancel.cancelled()) throw utils::HttpClientCancellationException(42, "Request cancelled by client");
}
bool invocation_active(const std::weak_ptr<const std::atomic<bool>>& lifetime) {
    const auto current = lifetime.lock();
    return current && current->load(std::memory_order_acquire);
}
typed::Platform typed_platform(const std::string& os) {
    if (os == "windows") return typed::Platform::Windows;
    if (os == "linux") return typed::Platform::Linux;
    if (os == "macos") return typed::Platform::Macos;
    throw std::invalid_argument("Unknown runtime platform");
}
InstallFactSources install_fact_sources() {
    return {
        [] {
            const auto config = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
            if (!config.contains("therock") || !config["therock"].is_object() || !config["therock"].contains("version") || !config["therock"]["version"].is_string())
                throw std::runtime_error("backend_versions.json is missing 'therock.version'");
            return config["therock"]["version"].get<std::string>();
        },
        [] {
            const auto family = SystemInfo::rocm_asset_family(SystemInfo::get_rocm_arch());
            if (family.empty()) throw std::runtime_error(SystemInfo::get_unsupported_backend_error("llamacpp", "rocm-nightly"));
            return family;
        },
        [] {
            const auto architecture = SystemInfo::get_cuda_arch();
            if (architecture.empty()) throw std::runtime_error(SystemInfo::get_unsupported_backend_error("llamacpp", "cuda"));
            return architecture;
        },
        [] { const auto* config = RuntimeConfig::global(); return config ? config->rocm_channel() : "stable"; }
    };
}
InstallServices::InstallServices(InvocationGuard& guard, typed::InstallFactsInput input, InstallFactSources sources)
    : guard_(guard), input_(std::move(input)), sources_(std::move(sources)) {}
std::string InstallServices::read(typed::InstallFact fact, const std::string& backend, const std::function<std::string()>& source) {
    guard_.check(typed::Effect::InstallFactRead);
    if ((input_.phase != typed::InstallPhase::Asset &&
        std::find(input_.repository_facts.begin(), input_.repository_facts.end(), fact) == input_.repository_facts.end()) || input_.backend != backend ||
        std::find(input_.requested.begin(), input_.requested.end(), fact) == input_.requested.end())
        throw std::logic_error("Install fact outside selected phase/case");
    const auto result = source();
    guard_.check(typed::Effect::InstallFactRead, result.size());
    return result;
}
std::string InstallServices::therock_version() { return read(typed::InstallFact::TheRockVersion, "rocm-stable", sources_.therock_version); }
std::string InstallServices::rocm_asset_family() { return read(typed::InstallFact::RocmAssetFamily, "rocm-nightly", sources_.rocm_asset_family); }
std::string InstallServices::cuda_architecture() { return read(typed::InstallFact::CudaArchitecture, "cuda", sources_.cuda_architecture); }

namespace {
std::string system_executable() {
    const auto* path = std::getenv("PATH");
    if (!path) return "";
    const std::string paths(path);
#ifdef _WIN32
    const char separator = ';';
    const char* binary = "llama-server.exe";
#else
    const char separator = ':';
    const char* binary = "llama-server";
#endif
    std::size_t start = 0;
    do {
        const auto end = paths.find(separator, start);
        const auto directory = utils::path_from_utf8(paths.substr(start, end - start));
        if (directory.is_absolute()) {
            std::error_code error;
            const auto candidate = std::filesystem::canonical(directory / binary, error);
            if (!error && std::filesystem::is_regular_file(candidate, error)) return utils::path_to_utf8(candidate);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return "";
}
}
ProbeSources probe_sources() {
    return {
        [](std::size_t bytes, unsigned wait, const std::function<bool()>& cancel) {
            const auto executable = system_executable();
            return executable.empty() ? utils::CapturedProcess{} : utils::ProcessManager::capture_output(executable, {"--version"}, bytes, wait, cancel);
        },
        [] {
#ifdef __linux__
            std::error_code error;
            return std::filesystem::exists("/sys/class/kfd", error);
#else
            return false;
#endif
        },
        [] {
#ifdef __linux__
            std::vector<std::filesystem::path> paths{
                "/usr/lib/x86_64-linux-gnu/ggml/backends0/libggml-hip.so", "/usr/lib/libggml-hip.so",
                "/usr/lib/ggml/libggml-hip.so", "/usr/lib64/ggml/libggml-hip.so", "/usr/lib/ggml/backends0/libggml-hip.so",
                "/usr/lib64/ggml/backends0/libggml-hip.so"};
            const auto binary = system_executable();
            if (!binary.empty()) {
                const auto directory = utils::path_from_utf8(binary).parent_path();
                paths.push_back(directory / "libggml-hip.so");
                paths.push_back(directory.parent_path() / "lib" / "libggml-hip.so");
            }
            for (const auto& path : paths) {
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) return true;
            }
#endif
            return false;
        }
    };
}
SystemServices::SystemServices(InvocationGuard& guard, typed::SystemInstallInput input, ProbeSources sources)
    : guard_(guard), input_(std::move(input)), sources_(std::move(sources)) {}
void SystemServices::check(bool linux_only) {
    guard_.check(typed::Effect::SystemProbe);
    if (input_.backend != "system" || (linux_only && (!input_.binary_found || input_.platform != typed::Platform::Linux)))
        throw std::logic_error("System probe outside eligible selected backend");
}
std::string SystemServices::llama_version() {
    check();
    const auto result = sources_.version(65536, 5000, [&] { guard_.check_active(); return false; });
    guard_.check(typed::Effect::SystemProbe, result.output.size());
    if (result.byte_limit || result.timed_out || result.cancelled || result.exit_code != 0) throw std::runtime_error("Bounded version probe failed");
    std::smatch match;
    const std::regex version(R"(version:\s*(\d+)|version\s+b?(\d+))");
    if (std::regex_search(result.output, match, version)) return "b" + (match[1].matched ? match[1].str() : match[2].str());
    return result.output.empty() ? "unknown" : "detected";
}
bool SystemServices::kfd_present() { check(true); return sources_.kfd(); }
bool SystemServices::hip_plugin_present() { check(true); return sources_.hip_plugin(); }

EnvironmentSources environment_sources() {
    const auto rocm_arch = std::make_shared<std::string>();
    const auto cuda_arch = std::make_shared<std::string>();
    return {
        [](typed::ParentKey key) -> std::optional<std::string> {
            const char* name = nullptr;
            switch (key) {
                case typed::ParentKey::Path: name = "PATH"; break;
                case typed::ParentKey::LlamaDevice: name = "LLAMA_ARG_DEVICE"; break;
                case typed::ParentKey::MetalNoResidency: name = "GGML_METAL_NO_RESIDENCY"; break;
                case typed::ParentKey::Home: name = "HOME"; break;
                case typed::ParentKey::LibraryPath: name = "LD_LIBRARY_PATH"; break;
                case typed::ParentKey::CudaVisibleDevices: name = "CUDA_VISIBLE_DEVICES"; break;
                case typed::ParentKey::PrimeRenderOffload: name = "__NV_PRIME_RENDER_OFFLOAD"; break;
                default: throw std::logic_error("Unknown enumerated parent key");
            }
            const auto* value = std::getenv(name);
            return value ? std::optional<std::string>(utils::get_environment_variable_utf8(name)) : std::nullopt;
        },
        [rocm_arch, cuda_arch](typed::RuntimeQuery query) {
            switch (query) {
                case typed::RuntimeQuery::RocmArchitecture: return *rocm_arch = SystemInfo::get_rocm_arch();
                case typed::RuntimeQuery::CudaArchitecture: return *cuda_arch = SystemInfo::get_cuda_arch();
                case typed::RuntimeQuery::TheRockDirectory: return BackendUtils::join_runtime_dirs(BackendUtils::get_therock_lib_paths(*rocm_arch));
                case typed::RuntimeQuery::CudaVisibleDevices: return SystemInfo::get_cuda_visible_devices_for_arch(*cuda_arch);
                case typed::RuntimeQuery::HomeDirectory: {
#ifdef _WIN32
                    return utils::get_environment_variable_utf8("USERPROFILE");
#else
                    char buffer[16384];
                    struct passwd record{};
                    struct passwd* result = nullptr;
                    if (getpwuid_r(getuid(), &record, buffer, sizeof(buffer), &result) == 0 && result && record.pw_dir) return std::string(record.pw_dir);
                    return std::string();
#endif
                }
                default: throw std::logic_error("Unknown enumerated runtime query");
            }
        },
        [](const std::string& architecture, const std::string& destination) {
            BackendUtils::stage_therock_hip_runtime(architecture, utils::path_from_utf8(destination));
        }
    };
}
EnvironmentServices::EnvironmentServices(InvocationGuard& guard, EnvironmentSources sources, std::string destination, std::optional<typed::StagingScope> scope)
    : guard_(guard), sources_(std::move(sources)), destination_(std::move(destination)), scope_(scope) {}
std::optional<typed::StagingCapability> EnvironmentServices::capability() {
    guard_.check(typed::Effect::StagingPlan);
    return scope_ ? std::optional<typed::StagingCapability>(staging_capability(*scope_)) : std::nullopt;
}
std::optional<std::string> EnvironmentServices::parent(typed::ParentKey key) {
    guard_.check(typed::Effect::ParentEnvironmentRead);
    if (key != typed::ParentKey::Path && key != typed::ParentKey::LlamaDevice && key != typed::ParentKey::MetalNoResidency && key != typed::ParentKey::Home && key != typed::ParentKey::LibraryPath &&
        key != typed::ParentKey::CudaVisibleDevices && key != typed::ParentKey::PrimeRenderOffload)
        throw std::logic_error("Unknown enumerated parent key");
    auto found = parents_.find(key);
    if (found == parents_.end()) found = parents_.emplace(key, sources_.parent(key)).first;
    const auto& result = found->second;
    guard_.check(typed::Effect::ParentEnvironmentRead, result ? result->size() : 0);
    return result;
}
std::string EnvironmentServices::runtime(typed::RuntimeQuery query) {
    guard_.check(typed::Effect::RuntimeFactRead);
    if (query != typed::RuntimeQuery::RocmArchitecture && query != typed::RuntimeQuery::TheRockDirectory &&
        query != typed::RuntimeQuery::CudaVisibleDevices && query != typed::RuntimeQuery::HomeDirectory && query != typed::RuntimeQuery::CudaArchitecture)
        throw std::logic_error("Unknown enumerated runtime query");
    const auto result = sources_.runtime(query);
    guard_.check(typed::Effect::RuntimeFactRead, result.size());
    if (query == typed::RuntimeQuery::RocmArchitecture) architectures_.insert(result);
    return result;
}
void EnvironmentServices::stage(const typed::StagingAction& action) {
    typed::EnvironmentPlan plan;
    plan.staging.push_back(action);
    StagingConsumer::execute(seal(std::move(plan)));
}
StagingPlan EnvironmentServices::seal(typed::EnvironmentPlan plan) {
    guard_.check_active();
    for (const auto& action : plan.staging) {
        guard_.check(typed::Effect::StagingPlan);
        if (!scope_ || destination_.empty() || !owns(action.capability) || !architectures_.count(action.architecture))
            throw std::logic_error("Staging action outside issued capability");
    }
    for (const auto& [name, value] : plan.assignments) {
        if (name.empty() || name.find_first_of("=\r\n") != std::string::npos || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            throw std::logic_error("Invalid child environment assignment");
        guard_.check(typed::Effect::ParentEnvironmentRead, name.size() + value.size());
    }
    return StagingPlan(guard_, std::move(plan), destination_, sources_.stage);
}
std::vector<std::pair<std::string, std::string>> StagingConsumer::execute(StagingPlan plan) {
    if (!invocation_active(plan.lifetime_)) throw std::logic_error("Expired staging invocation");
    plan.guard_->check_active();
    for (const auto& action : plan.plan_.staging) {
        plan.guard_->check(typed::Effect::StagingPlan);
        plan.stage_(action.architecture, plan.destination_);
        plan.guard_->check_active();
    }
    return std::move(plan.plan_.assignments);
}
InvocationGuard::InvocationGuard(Frame& frame, const ExecutableDefinition& executable, std::size_t slot,
    const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
    const std::string& stage, utils::RequestCancelToken cancel)
    : frame_(frame), executable_(&executable), slot_(slot), owner_(hook.slot), nonce_(frame.nonce),
      effects_(hook.effects), thread_(std::this_thread::get_id()), cancel_(std::move(cancel)) {
    check_cancelled(cancel_);
    if (frame_.executable != executable_ || frame_.nonce != nonce_) throw std::logic_error("Foreign invocation frame");
    if (owner_.owner_kind != kind || owner_.owner_id != owner || owner_.stage != stage)
        throw std::logic_error("Bound slot owner/stage mismatch");
    if (frame_.active_slot) throw std::logic_error("Nested bound slot invocation");
    for (const auto& input : hook.inputs) {
        if (input.producer ? !frame_.produced.count(*input.producer) : !frame_.facts.count(input.fact))
            throw std::logic_error("Missing bound slot fact");
    }
    for (const auto dependency : hook.dependencies) if (!frame_.produced.count(dependency))
        throw std::logic_error("Missing bound slot dependency");
    frame_.active_slot = slot_;
}
InvocationGuard::~InvocationGuard() { lifetime_->store(false, std::memory_order_release); active_ = false; frame_.active_slot.reset(); }
void InvocationGuard::check_active() const {
    if (!active_ || thread_ != std::this_thread::get_id() || frame_.executable != executable_ || frame_.nonce != nonce_ || frame_.active_slot != slot_)
        throw std::logic_error("Inactive bound slot invocation");
    check_cancelled(cancel_);
}
void InvocationGuard::check(typed::Effect effect, std::size_t allowance) {
    check_active();
    if (std::find(effects_.begin(), effects_.end(), effect) == effects_.end())
        throw std::logic_error("Effect outside bound slot");
    if (allowance > remaining_) throw std::length_error("Invocation service allowance exceeded");
    remaining_ -= allowance;
}
SelectionServices::SelectionServices(InvocationGuard& guard, std::vector<std::string> supported)
    : guard_(guard), supported_(std::move(supported)) {}
SelectionServices::SelectionServices(InvocationGuard& guard, std::function<void(const std::string&)> validate)
    : guard_(guard), validate_(std::move(validate)) {}
bool SelectionServices::supports(const std::string& backend) {
    guard_.check(typed::Effect::SupportQuery);
    if (validate_) { validate_(backend); return true; }
    return std::find(supported_.begin(), supported_.end(), backend) != supported_.end();
}
typed::DeviceCheck SelectionServices::validate_device(const typed::DeviceValidationInput& input) {
    guard_.check(typed::Effect::DeviceQuery);
    auto backend = input.backend;
    if (backend.rfind("rocm-", 0) == 0) backend = "rocm";
    if (std::find(supported_.begin(), supported_.end(), backend) == supported_.end() &&
        std::find(supported_.begin(), supported_.end(), input.backend) == supported_.end())
        throw std::logic_error("Device query outside selected backend scope");
    try { BackendUtils::validate_device_backend_match(input.backend, input.device); return {true, ""}; }
    catch (const std::invalid_argument& error) { return {false, error.what()}; }
}

}
