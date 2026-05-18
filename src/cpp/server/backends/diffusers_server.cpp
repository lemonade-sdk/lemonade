#include "lemon/backends/diffusers_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <sstream>

namespace lemon {
namespace backends {

InstallParams DiffusersServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "rocm") {
        params.repo = "lemonade-sdk/diffusers-rocm";
        std::string target_arch = SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("diffusers", "rocm")
            );
        }
#ifdef __linux__
        // Release tag: {version}-{target_arch}, e.g.
        // diffusers0.35.0-rocm7.12.0-gfx1151
        std::string release_tag = version + "-" + target_arch;
        params.version_override = release_tag;
        params.filename = release_tag + "-x64.tar.gz";
#else
        throw std::runtime_error("Diffusers ROCm is only supported on Linux");
#endif
    } else {
        throw std::runtime_error("Diffusers backend '" + backend + "' is not supported. Supported: rocm");
    }

    return params;
}

DiffusersServer::DiffusersServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("diffusers-server", log_level, model_manager, backend_manager) {
}

DiffusersServer::~DiffusersServer() {
    unload();
}

void DiffusersServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    LOG(INFO, "Diffusers") << "Loading model: " << model_name << std::endl;

    std::string diffusers_backend = options.get_option("diffusers_backend");
    std::string diffusers_args = options.get_option("diffusers_args");

    RuntimeConfig::validate_backend_choice("diffusers", diffusers_backend);

    backend_manager_->install_backend(SPEC.recipe, diffusers_backend);

    // The `checkpoint` field is the HuggingFace model id.
    std::string model_id = model_info.checkpoint();
    if (model_id.empty()) {
        throw std::runtime_error("Model checkpoint (HuggingFace ID) not found for: " + model_name);
    }

    LOG(DEBUG, "Diffusers") << "Using model: " << model_id << std::endl;

    port_ = choose_port();

    std::string executable = BackendUtils::get_backend_binary_path(SPEC, diffusers_backend);

    std::vector<std::string> args;
    args.push_back("--model");
    args.push_back(model_id);
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    args.push_back("--host");
    args.push_back("127.0.0.1");
    args.push_back("--served-model-name");
    args.push_back(model_name);

    // Optional: GGUF filename + base repo can be passed via diffusers_args from
    // server_models.json. Example diffusers_args:
    //   "--gguf-file flux1-schnell-Q4_K_M.gguf --base-repo black-forest-labs/FLUX.1-schnell"
    if (!diffusers_args.empty()) {
        LOG(DEBUG, "Diffusers") << "Adding custom arguments: " << diffusers_args << std::endl;
        std::istringstream iss(diffusers_args);
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }
    }

    LOG(INFO, "Diffusers") << "Starting diffusers-server on port " << port_ << "..." << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;
    // Triton SDPA flash-attention on ROCm
    env_vars.push_back({"FLASH_ATTENTION_TRITON_AMD_ENABLE", "TRUE"});
    // Isolate bundled site-packages from system/user Python
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = utils::ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars);

    // First-load can be slow: HF download of the GGUF (~5GB for FLUX-schnell Q4)
    // plus transformer load + pipeline assembly. Give it a 15-minute ceiling.
    if (!wait_for_ready("/health", 900)) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        std::string err = "diffusers-server failed to start within timeout";
        if (needs_gfx1151_cwsr_fix()) {
            err += ". Your kernel may be missing the gfx1151 CWSR fix — "
                   "see https://lemonade-server.ai/gfx1151_linux.html";
        }
        throw std::runtime_error(err);
    }

    LOG(DEBUG, "Diffusers") << "Model loaded on port " << port_ << std::endl;
}

void DiffusersServer::unload() {
    LOG(INFO, "Diffusers") << "Unloading model..." << std::endl;
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
}

// Diffusers is image-only — completion-style methods return clear errors.
static json not_supported(const std::string& endpoint) {
    return json{
        {"error", json{
            {"message", "Diffusers backend does not support " + endpoint +
                        ". Use /v1/images/generations instead."},
            {"type", "invalid_request_error"},
            {"code", "endpoint_not_supported"},
        }}
    };
}

json DiffusersServer::chat_completion(const json& /*request*/) {
    return not_supported("chat completions");
}

json DiffusersServer::completion(const json& /*request*/) {
    return not_supported("text completions");
}

json DiffusersServer::responses(const json& /*request*/) {
    return not_supported("responses");
}

json DiffusersServer::image_generations(const json& request) {
    return forward_request("/v1/images/generations", request);
}

json DiffusersServer::image_edits(const json& /*request*/) {
    // Phase 3: FluxKontextPipeline + Qwen-Image-Edit
    return not_supported("image edits");
}

json DiffusersServer::image_variations(const json& /*request*/) {
    // Phase 3
    return not_supported("image variations");
}

} // namespace backends
} // namespace lemon
