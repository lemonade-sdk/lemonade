#include "../backend_typed_hook_registry_internal.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace lemon::backends::typed::llamacpp {
namespace {

bool labeled(const std::vector<std::string>& labels, const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

bool dflash(std::string checkpoint) {
    std::transform(checkpoint.begin(), checkpoint.end(), checkpoint.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto separator = checkpoint.find_last_of("/:\\");
    const auto name = separator == std::string::npos ? checkpoint : checkpoint.substr(separator + 1);
    return name.rfind("dflash-", 0) == 0 || name == "dflash.gguf";
}

}

BackendSelection select_backend(const BackendSelectionInput& input, BackendSelectionServices& services) {
    LEMON_TYPED_ENTRY("backend.select_backend");
    auto backend = input.backend;
    if (backend.empty()) {
        const auto found = std::find_if(input.supported.begin(), input.supported.end(),
            [&](const auto& value) { return services.supports(value); });
        if (found == input.supported.end()) throw std::runtime_error("No supported llama.cpp backend");
        backend = *found;
    } else if (!services.supports(backend)) {
        throw std::runtime_error("Unsupported llama.cpp backend: " + backend);
    }
    if (backend == "rocm") {
        if (input.rocm_channel != "stable" && input.rocm_channel != "nightly") throw std::logic_error("unknown ROCm channel");
        backend += "-" + input.rocm_channel;
    }
    return {backend, backend != "cpu"};
}

DeviceCheck validate_device(const DeviceValidationInput& input, BackendSelectionServices& services) {
    LEMON_TYPED_ENTRY("backend.validate_device");
    return input.device.empty() ? DeviceCheck{true, ""} : services.validate_device(input);
}

InstallFacts install_context(const InstallFactsInput& input, InstallFactsServices& services) {
    LEMON_TYPED_ENTRY("install_context");
    InstallFacts result;
    for (const auto fact : input.requested) {
        if (input.phase != InstallPhase::Asset &&
            std::find(input.repository_facts.begin(), input.repository_facts.end(), fact) == input.repository_facts.end())
            throw std::logic_error("install fact requested before asset phase");
        switch (fact) {
            case InstallFact::TheRockVersion: {
                if (input.backend != "rocm-stable") throw std::logic_error("TheRock fact for wrong backend");
                auto version = services.therock_version();
                if (!version.empty() && version.front() == 'v') version.erase(0, 1);
                const auto first = version.find('.');
                const auto second = first == std::string::npos ? first : version.find('.', first + 1);
                result.therock_version = version.substr(0, second);
                break;
            }
            case InstallFact::RocmAssetFamily:
                if (input.backend != "rocm-nightly") throw std::logic_error("ROCm fact for wrong backend");
                result.rocm_asset_family = services.rocm_asset_family();
                break;
            case InstallFact::CudaArchitecture:
                if (input.backend != "cuda") throw std::logic_error("CUDA fact for wrong backend");
                result.cuda_architecture = services.cuda_architecture();
                break;
            default: throw std::logic_error("unknown install fact");
        }
    }
    return result;
}

RuntimeFacts runtime_options(const RuntimeOptionsInput& input) {
    LEMON_TYPED_ENTRY("runtime_options");
    const bool use_dflash = !input.draft_checkpoint.empty() && dflash(input.draft_checkpoint) && labeled(input.labels, "dflash");
    return {use_dflash, !use_dflash && labeled(input.labels, "mtp")};
}

ModelFacts prepare(const PrepareInput& input) {
    LEMON_TYPED_ENTRY("prepare");
    ModelFacts result;
    result.hf_load = input.hf_load;
    result.embedding = input.model_type == ModelType::EMBEDDING;
    result.reranking = input.model_type == ModelType::RERANKING;
    result.context_size = input.context_size;
    result.use_mmproj = !input.hf_load && !input.mmproj_path.empty();
    result.mmproj_on_cpu = result.use_mmproj && !input.use_gpu;
    result.use_draft_checkpoint = !input.draft_path.empty() &&
        (!dflash(input.draft_checkpoint) || labeled(input.labels, "dflash"));
    return result;
}

EnvironmentPlan environment(const EnvironmentInput& input, EnvironmentServices& services) {
    LEMON_TYPED_ENTRY("environment");
    if (input.staging && !services.owns(*input.staging)) throw std::logic_error("foreign staging capability");
    EnvironmentPlan result;
    const bool rocm = input.backend == "rocm-stable" || input.backend == "rocm-nightly";
    const bool windows = input.platform == Platform::Windows;
    if (rocm || input.backend == "cuda") {
        std::string directories = windows && rocm ? "" : input.executable_directory;
        if (input.backend == "rocm-stable") {
            const auto architecture = services.runtime(RuntimeQuery::RocmArchitecture);
            if (!architecture.empty()) {
                const auto runtime = services.runtime(RuntimeQuery::TheRockDirectory);
                if (!runtime.empty()) directories = runtime + (directories.empty() ? "" : (windows ? ";" : ":") + directories);
            }
        }
        if (!directories.empty()) {
            const auto path = services.parent(windows ? ParentKey::Path : ParentKey::LibraryPath).value_or("");
            result.assignments.emplace_back(windows ? "PATH" : "LD_LIBRARY_PATH", directories + (path.empty() ? "" : (windows ? ";" : ":") + path));
        }
        if (windows && rocm) {
            if (input.backend == "rocm-stable") {
                const auto architecture = services.runtime(RuntimeQuery::RocmArchitecture);
                if (!architecture.empty() && input.staging) result.staging.push_back({*input.staging, architecture});
            }
            const auto architecture = services.runtime(RuntimeQuery::RocmArchitecture);
            if (architecture == "gfx1151" || architecture == "gfx1152") result.assignments.emplace_back("OCL_SET_SVM_SIZE", "262144");
        }
    }
    if (input.backend == "cuda") {
        const auto override = services.parent(ParentKey::LlamaDevice).value_or("");
        if (input.device.empty() && override.empty() && services.parent(ParentKey::CudaVisibleDevices).value_or("").empty()) {
            const auto architecture = services.runtime(RuntimeQuery::CudaArchitecture);
            const auto devices = services.runtime(RuntimeQuery::CudaVisibleDevices);
            if (!architecture.empty() && !devices.empty()) result.assignments.emplace_back("CUDA_VISIBLE_DEVICES", devices);
        }
        if (input.platform == Platform::Linux && services.parent(ParentKey::PrimeRenderOffload).value_or("").empty())
            result.assignments.emplace_back("__NV_PRIME_RENDER_OFFLOAD", "1");
    }
    if (input.platform == Platform::Macos) {
        const auto residency = services.parent(ParentKey::MetalNoResidency);
        if (residency) result.assignments.emplace_back("GGML_METAL_NO_RESIDENCY", *residency);
        if (services.parent(ParentKey::Home).value_or("").empty()) {
            const auto home = services.runtime(RuntimeQuery::HomeDirectory);
            result.assignments.emplace_back("HOME", home.empty() ? "/var/root" : home);
        }
    }
    return result;
}

std::string resolve_version(const SystemVersionInput& input, SystemProbeServices& services) {
    LEMON_TYPED_ENTRY("system.resolve_version");
    return input.backend == "system" ? services.llama_version() : input.file_version;
}

SystemInstallCheck check_install(const SystemInstallInput& input, SystemProbeServices& services) {
    LEMON_TYPED_ENTRY("system.check_install");
    if (input.binary_found && input.backend == "system" && input.platform == Platform::Linux &&
        services.kfd_present() && !services.hip_plugin_present()) return {false, "HIP plugin libggml-hip.so not installed"};
    return {input.binary_found, ""};
}

nlohmann::json max_tokens_alias(nlohmann::json value) {
    LEMON_TYPED_ENTRY("max_tokens_alias");
    return ::lemon::backends::llamacpp::with_max_tokens_alias(std::move(value));
}

nlohmann::json tool_schema_limits(nlohmann::json value) {
    LEMON_TYPED_ENTRY("tool_schema_limits");
    return ::lemon::backends::llamacpp::sanitize_tool_schema_limits(std::move(value));
}

nlohmann::json client_model(nlohmann::json value, const nlohmann::json& request, const std::string& model) {
    LEMON_TYPED_ENTRY("client_model");
    return ::lemon::backends::llamacpp::normalize_response_model(std::move(value), request, model);
}

}
