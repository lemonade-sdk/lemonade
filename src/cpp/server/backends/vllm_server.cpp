#include "lemon/backends/vllm_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/vllm_arg_resolver.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <cstdint>

using namespace lemon::utils;

namespace lemon {
namespace backends {

static constexpr int64_t ANTHROPIC_MAX_TOKENS_PREFLIGHT_THRESHOLD = 8192;

InstallParams VLLMServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "rocm") {
        params.repo = "lemonade-sdk/vllm-rocm";
        std::string target_arch = SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("vllm", "rocm")
            );
        }
#ifdef __linux__
        // One release per GPU target since 0.19.1: release tag is
        // {version}-{target_arch}, e.g. vllm0.20.1-rocm7.12.0-gfx1151.
        std::string release_tag = version + "-" + target_arch;
        params.version_override = release_tag;
        params.filename = release_tag + "-x64.tar.gz";
#else
        throw std::runtime_error("vLLM ROCm is only supported on Linux");
#endif
    } else {
        throw std::runtime_error("vLLM backend '" + backend + "' is not supported. Supported: rocm");
    }

    return params;
}

VLLMServer::VLLMServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("vllm-server", log_level, model_manager, backend_manager) {
}

VLLMServer::~VLLMServer() {
    unload();
}

void VLLMServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    LOG(INFO, "vLLM") << "Loading model: " << model_name << std::endl;

    std::string vllm_backend = options.get_option("vllm_backend");
    std::string vllm_args = options.get_option("vllm_args");
    int ctx_size = options.get_option("ctx_size");
    max_model_len_ = ctx_size;

    RuntimeConfig::validate_backend_choice("vllm", vllm_backend);

    // vLLM uses HuggingFace model names, not local file paths.
    // The checkpoint field in server_models.json is the HF model ID.
    std::string model_id = model_info.checkpoint();
    if (model_id.empty()) {
        throw std::runtime_error("Model checkpoint (HuggingFace ID) not found for: " + model_name);
    }

    LOG(DEBUG, "vLLM") << "Using model: " << model_id << std::endl;

    json vllm_model_config =
        JsonUtils::load_from_file(utils::get_resource_path("resources/vllm_model_config.json"));
    VLLMArgResolution resolved_vllm_args = resolve_vllm_args(model_name, model_id, vllm_model_config, vllm_args);

    // Install vllm-server if needed
    backend_manager_->install_backend(SPEC.recipe, vllm_backend);

    // Choose port
    port_ = choose_port();

    // Get executable path
    std::string executable = BackendUtils::get_backend_binary_path(SPEC, vllm_backend);

    // Build command line arguments
    std::vector<std::string> args;
    args.push_back("--model");
    args.push_back(model_id);
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    args.push_back("--host");
    args.push_back("127.0.0.1");
    // Serve using the Lemonade model name so forwarded requests match
    args.push_back("--served-model-name");
    args.push_back(model_name);
    // Keep eager execution for consumer GPU inference; leave dtype selection to vLLM.
    args.push_back("--enforce-eager");
    // Pass ctx_size through to vllm-server's --max-model-len. Trust the
    // user's value verbatim; the global default lives in defaults.json
    // (same as llamacpp). Larger values raise KV-cache memory and Triton
    // JIT compile time.
    args.push_back("--max-model-len");
    args.push_back(std::to_string(ctx_size));

    args.push_back("--enable-prefix-caching");

    // Avoid vLLM's default gpu_memory_utilization=0.92 on shared-memory systems.
    // User-provided memory-budget args deliberately suppress this code default.
    if (!resolved_vllm_args.user_has_memory_budget_arg) {
        args.push_back("--kv-cache-memory-bytes");
        args.push_back("4G");
    }

    if (!resolved_vllm_args.args.empty()) {
        LOG(DEBUG, "vLLM") << "Adding model/user arguments from vLLM resolver" << std::endl;
        args.insert(args.end(), resolved_vllm_args.args.begin(), resolved_vllm_args.args.end());
    }

    LOG(INFO, "vLLM") << "Starting vllm-server on port " << port_ << "..." << std::endl;

    // Set environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;

    // The vllm-server launcher script handles LD_LIBRARY_PATH for ROCm libs.
    // Set FLASH_ATTENTION_TRITON_AMD_ENABLE for ROCm flash attention.
    env_vars.push_back({"FLASH_ATTENTION_TRITON_AMD_ENABLE", "TRUE"});
    // Prevent system/user Python packages from leaking into the bundled vLLM environment
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    // Start process
    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars);

    // vLLM can take longer to start (loading model, compiling kernels)
    if (!wait_for_ready("/health", 600)) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        std::string err = "vllm-server failed to start within timeout";
        // A common cause on gfx1151 is a kernel without the CWSR fix, which makes
        // any GPU dispatch hang or fault. Point users to the docs in that case.
        if (needs_gfx1151_cwsr_fix()) {
            err += ". Your kernel may be missing the gfx1151 CWSR fix — "
                   "see https://lemonade-server.ai/gfx1151_linux.html";
        }
        max_model_len_ = 0;
        throw std::runtime_error(err);
    }

    LOG(DEBUG, "vLLM") << "Model loaded on port " << port_ << std::endl;
}

void VLLMServer::unload() {
    LOG(INFO, "vLLM") << "Unloading model..." << std::endl;
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        max_model_len_ = 0;
    }
}

json VLLMServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json VLLMServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json VLLMServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

json VLLMServer::anthropic_messages(const json& request) {
    return forward_anthropic_messages(fit_anthropic_max_tokens_to_context(request));
}

void VLLMServer::anthropic_messages_stream(const std::string& request_body, httplib::DataSink& sink) {
    try {
        json request = json::parse(request_body);
        forward_anthropic_messages_stream(fit_anthropic_max_tokens_to_context(request).dump(), sink);
    } catch (const std::exception& e) {
        LOG(WARNING, "vLLM") << "Failed to inspect Anthropic stream request: "
                             << e.what() << "; forwarding unchanged" << std::endl;
        forward_anthropic_messages_stream(request_body, sink);
    }
}

json VLLMServer::anthropic_count_tokens(const json& request) {
    return forward_anthropic_count_tokens(request);
}

json VLLMServer::fit_anthropic_max_tokens_to_context(const json& request) {
    if (max_model_len_ <= 0 ||
        !request.contains("max_tokens") ||
        (!request["max_tokens"].is_number_integer() &&
         !request["max_tokens"].is_number_unsigned())) {
        return request;
    }

    int64_t requested_max_tokens = request["max_tokens"].get<int64_t>();
    if (requested_max_tokens <= 0 ||
        requested_max_tokens <= ANTHROPIC_MAX_TOKENS_PREFLIGHT_THRESHOLD) {
        return request;
    }

    json count_request = request;
    count_request.erase("max_tokens");
    count_request.erase("stream");

    auto raw = forward_request_raw("/v1/messages/count_tokens", count_request);
    if (raw.status_code != 200) {
        LOG(DEBUG, "vLLM") << "Skipping Anthropic max_tokens fit; count_tokens returned HTTP "
                           << raw.status_code << std::endl;
        return request;
    }

    json count_response;
    try {
        count_response = json::parse(raw.body);
    } catch (const std::exception& e) {
        LOG(DEBUG, "vLLM") << "Skipping Anthropic max_tokens fit; count_tokens parse failed: "
                           << e.what() << std::endl;
        return request;
    }

    if (!count_response.contains("input_tokens") ||
        (!count_response["input_tokens"].is_number_integer() &&
         !count_response["input_tokens"].is_number_unsigned())) {
        return request;
    }

    int64_t input_tokens = count_response["input_tokens"].get<int64_t>();
    int64_t available_output_tokens = max_model_len_ - input_tokens;
    if (available_output_tokens <= 0 || requested_max_tokens <= available_output_tokens) {
        return request;
    }

    json modified_request = request;
    modified_request["max_tokens"] = available_output_tokens;
    LOG(INFO, "vLLM") << "Reduced Anthropic max_tokens from " << requested_max_tokens
                      << " to " << available_output_tokens
                      << " so input_tokens (" << input_tokens
                      << ") fits max_model_len (" << max_model_len_ << ")" << std::endl;
    return modified_request;
}

} // namespace backends
} // namespace lemon
