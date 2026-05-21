#include "lemon/backends/mlx_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// Resolve a user-facing backend choice ("rocm", "auto") to the concrete
// variant used in asset filenames and install directories.
static std::string resolve_mlx_backend(const std::string& backend) {
    if (backend == "auto" || backend.empty()) {
#ifdef __APPLE__
        return "metal";
#else
        // On non-Apple platforms, default to rocm if an AMD GPU is present,
        // fall back to cpu otherwise. SystemInfo already knows the truth.
        std::string arch = SystemInfo::get_rocm_arch();
        return arch.empty() ? "cpu" : "rocm";
#endif
    }
    return backend;
}

static std::vector<std::string> tokenize_quoted_args(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    bool token_started = false;

    for (char ch : input) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            token_started = true;
            continue;
        }

        if (ch == '\\' && !in_single_quote) {
            escaped = true;
            token_started = true;
            continue;
        }

        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            token_started = true;
            continue;
        }

        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            token_started = true;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) && !in_single_quote && !in_double_quote) {
            if (token_started) {
                tokens.push_back(current);
                current.clear();
                token_started = false;
            }
            continue;
        }

        current.push_back(ch);
        token_started = true;
    }

    if (escaped) {
        throw std::runtime_error("Invalid lemon-mlx args: trailing escape");
    }
    if (in_single_quote || in_double_quote) {
        throw std::runtime_error("Invalid lemon-mlx args: unmatched quote");
    }
    if (token_started) {
        tokens.push_back(current);
    }

    return tokens;
}

InstallParams MlxServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/lemon-mlx-engine";

    const std::string resolved = resolve_mlx_backend(backend);

    if (resolved == "system") {
        return params;
    }

    if (resolved == "metal") {
#ifdef __APPLE__
        params.filename = "mlx-engine-" + version + "-macos-arm64.zip";
#else
        throw std::runtime_error("Metal lemon-mlx is only supported on macOS");
#endif
    } else if (resolved == "rocm") {
#ifdef __linux__
        std::string arch = SystemInfo::get_rocm_arch();
        if (arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("lemon-mlx", "rocm")
            );
        }
        params.filename = "mlx-engine-" + version + "-ubuntu-rocm-stable-" + arch + "-x64.zip";
#else
        throw std::runtime_error("ROCm lemon-mlx is only supported on Linux");
#endif
    } else if (resolved == "cpu") {
#ifdef __linux__
#if !defined(__x86_64__) && !defined(_M_X64)
        throw std::runtime_error(
            "CPU lemon-mlx is only supported on Linux x86_64; "
            "no Linux arm64 build of lemon-mlx-engine is available");
#endif
        params.filename = "mlx-engine-" + version + "-ubuntu-cpu-x64.zip";
#elif defined(__APPLE__)
        // On macOS the "cpu" build is served by the macos-arm64 asset
        // (MLX runs through Metal/Accelerate even with no explicit GPU selection).
        params.filename = "mlx-engine-" + version + "-macos-arm64.zip";
#else
        throw std::runtime_error("CPU lemon-mlx is not supported on this platform");
#endif
    } else {
        throw std::runtime_error("Unknown lemon-mlx backend: " + resolved);
    }

    return params;
}

MlxServer::MlxServer(const std::string& log_level,
                     ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("lemon-mlx", log_level, model_manager, backend_manager) {
}

MlxServer::~MlxServer() {
    unload();
}

void MlxServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    LOG(INFO, "MLX") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "MLX") << "Per-model settings: " << options.to_log_string() << std::endl;

    int ctx_size = options.get_option("ctx_size");
    std::string mlx_backend_option = options.get_option("lemon-mlx_backend");
    std::string mlx_backend = resolve_mlx_backend(mlx_backend_option);
    std::string mlx_args = options.get_option("lemon-mlx_args");

    RuntimeConfig::validate_backend_choice("lemon-mlx", mlx_backend_option);

    LOG(INFO, "MLX") << "Using lemon-mlx backend: " << mlx_backend << std::endl;

    // The CPU build runs on CPU; everything else is GPU (Metal or ROCm).
    device_type_ = (mlx_backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;

    // Install lemon-mlx binary if needed.
    backend_manager_->install_backend(SPEC.recipe, mlx_backend);

    // Pass the HuggingFace repo-id (checkpoint) to lemon-mlx, not the
    // resolved local path. Upstream lemon-mlx unconditionally URL-joins the
    // model argument onto huggingface.co/api/models/, so an absolute
    // filesystem path produces a 404 like
    //   huggingface.co/api/models//home/.../hf-cache/...
    // The configured Hugging Face cache is passed to the lemon-mlx process
    // below, so a previously downloaded model is still resolved locally. The
    // resolved_path remains as a fallback for setups where the checkpoint
    // field is empty (rare; future custom-model recipes).
    std::string model_ref = model_info.checkpoint();
    if (model_ref.empty()) {
        model_ref = model_info.resolved_path();
    }
    if (model_ref.empty()) {
        throw std::runtime_error("lemon-mlx: no model path or checkpoint provided");
    }
    loaded_model_ref_ = model_ref;

    LOG(DEBUG, "MLX") << "Using model reference: " << model_ref << std::endl;

    port_ = choose_port();

    std::string executable = BackendUtils::get_backend_binary_path(SPEC, mlx_backend);

    std::vector<std::string> args;
    // Positional model argument — pre-load mode.
    args.push_back(model_ref);
    args.push_back("--host");
    args.push_back("127.0.0.1");
    args.push_back("--port");
    args.push_back(std::to_string(port_));

    if (ctx_size > 0) {
        args.push_back("--ctx-size");
        args.push_back(std::to_string(ctx_size));
    }

    // Honor custom user args last so they can override anything above.
    if (!mlx_args.empty()) {
        for (const auto& token : tokenize_quoted_args(mlx_args)) {
            args.push_back(token);
        }
    }

    LOG(INFO, "MLX") << "Starting lemon-mlx server..." << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;
    std::string hf_cache_dir = get_hf_cache_dir();
    if (!hf_cache_dir.empty()) {
        env_vars.push_back({"HF_HUB_CACHE", hf_cache_dir});
        LOG(DEBUG, "MLX") << "Setting HF_HUB_CACHE=" << hf_cache_dir << std::endl;
    }
#ifdef __linux__
    // Both Linux variants bundle their runtime libs next to the server binary
    // and the binary has no DT_RUNPATH, so without LD_LIBRARY_PATH the loader
    // can't find them and exits 127.
    //   rocm: libamdhip64.so, librocblas.so, ...
    //   cpu:  libopenblas.so.0, liblapacke.so.3, libgfortran.so.5
    {
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        const char* existing = std::getenv("LD_LIBRARY_PATH");
        if (existing && *existing) {
            lib_path = lib_path + ":" + std::string(existing);
        }
        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
        LOG(DEBUG, "MLX") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }

    if (mlx_backend == "cpu") {
        // The MLX CPU JIT emits generated C++ that can be rejected by newer
        // Linux GCC/libstdc++ combinations for _FloatN redeclarations. Disable
        // MLX compile for the CPU fallback; GPU backends keep their fast path.
        env_vars.push_back({"MLX_DISABLE_COMPILE", "1"});
        LOG(DEBUG, "MLX") << "Setting MLX_DISABLE_COMPILE=1 for CPU backend" << std::endl;
    }
#endif

    bool inherit = (log_level_ == "info") || is_debug();
    process_handle_ = ProcessManager::start_process(executable, args, "", inherit, true, env_vars);

    if (!wait_for_ready("/health")) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        throw std::runtime_error("lemon-mlx server failed to start");
    }

    LOG(DEBUG, "MLX") << "Model loaded on port " << port_ << std::endl;
}

void MlxServer::unload() {
    LOG(INFO, "MLX") << "Unloading model..." << std::endl;
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        loaded_model_ref_.clear();
    }
}

json MlxServer::prepare_request(const json& request) const {
    json modified = request;
    if (!loaded_model_ref_.empty()) {
        modified["model"] = loaded_model_ref_;
    }
    if (modified.contains("max_completion_tokens") && !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return modified;
}

json MlxServer::chat_completion(const json& request) {
    // OpenAI introduced `max_completion_tokens` to replace `max_tokens`
    // (Sep 2024). MLX only understands the older name.
    return forward_request("/v1/chat/completions", prepare_request(request));
}

json MlxServer::completion(const json& request) {
    return forward_request("/v1/completions", prepare_request(request));
}

json MlxServer::responses(const json& request) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "lemon-mlx")
    );
}

void MlxServer::forward_streaming_request(const std::string& endpoint,
                                          const std::string& request_body,
                                          httplib::DataSink& sink,
                                          bool sse,
                                          long timeout_seconds) {
    try {
        json request = json::parse(request_body);
        std::string modified_body = prepare_request(request).dump();
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse, timeout_seconds);
    } catch (const json::exception&) {
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds);
    }
}

} // namespace backends
} // namespace lemon
