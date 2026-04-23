#include "lemon/backends/mlx_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <cstdlib>
#include <filesystem>
#include <sstream>
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

static bool is_mlx_rocm_backend(const std::string& backend) {
    return backend == "rocm";
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
        throw std::runtime_error("Metal mlx-engine is only supported on macOS");
#endif
    } else if (resolved == "rocm") {
#ifdef __linux__
        std::string arch = SystemInfo::get_rocm_arch();
        if (arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("mlx-engine", "rocm")
            );
        }
        params.filename = "mlx-engine-" + version + "-ubuntu-rocm-stable-" + arch + "-x64.zip";
#else
        throw std::runtime_error("ROCm mlx-engine is only supported on Linux");
#endif
    } else if (resolved == "cpu") {
#ifdef __linux__
        params.filename = "mlx-engine-" + version + "-ubuntu-cpu-x64.zip";
#elif defined(__APPLE__)
        // On macOS the "cpu" build is served by the macos-arm64 asset
        // (MLX runs through Metal/Accelerate even with no explicit GPU selection).
        params.filename = "mlx-engine-" + version + "-macos-arm64.zip";
#else
        throw std::runtime_error("CPU mlx-engine is not supported on this platform");
#endif
    } else {
        throw std::runtime_error("Unknown mlx-engine backend: " + backend);
    }

    return params;
}

MlxServer::MlxServer(const std::string& log_level,
                     ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("mlx-engine", log_level, model_manager, backend_manager) {
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
    std::string mlx_backend_option = options.get_option("mlx_engine_backend");
    std::string mlx_backend = resolve_mlx_backend(mlx_backend_option);
    std::string mlx_args = options.get_option("mlx_engine_args");

    RuntimeConfig::validate_backend_choice("mlx-engine", mlx_backend_option);

    LOG(INFO, "MLX") << "Using mlx-engine backend: " << mlx_backend << std::endl;

    // The CPU build runs on CPU; everything else is GPU (Metal or ROCm).
    device_type_ = (mlx_backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;

    // Install mlx-engine binary if needed.
    backend_manager_->install_backend(SPEC.recipe, mlx_backend);

    // MLX identifies models by HuggingFace repo-id or a local directory path.
    // The ModelManager resolves local paths when available; fall back to the
    // checkpoint string (usually a repo-id) so the server auto-downloads on
    // first use.
    std::string model_ref = model_info.resolved_path();
    if (model_ref.empty()) {
        model_ref = model_info.checkpoint();
    }
    if (model_ref.empty()) {
        throw std::runtime_error("mlx-engine: no model path or checkpoint provided");
    }

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
        std::istringstream iss(mlx_args);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
    }

    LOG(INFO, "MLX") << "Starting mlx-engine server..." << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef __linux__
    if (is_mlx_rocm_backend(mlx_backend)) {
        // Point the loader at the bundled ROCm shared libraries shipped next
        // to the server binary (same pattern as llamacpp-rocm).
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        const char* existing = std::getenv("LD_LIBRARY_PATH");
        if (existing && *existing) {
            lib_path = lib_path + ":" + std::string(existing);
        }
        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
        LOG(DEBUG, "MLX") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#endif

    bool inherit = (log_level_ == "info") || is_debug();
    process_handle_ = ProcessManager::start_process(executable, args, "", inherit, true, env_vars);

    if (!wait_for_ready("/health")) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        throw std::runtime_error("mlx-engine server failed to start");
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
    }
}

json MlxServer::chat_completion(const json& request) {
    // OpenAI introduced `max_completion_tokens` to replace `max_tokens`
    // (Sep 2024). MLX only understands the older name.
    json modified = request;
    if (modified.contains("max_completion_tokens") && !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return forward_request("/v1/chat/completions", modified);
}

json MlxServer::completion(const json& request) {
    json modified = request;
    if (modified.contains("max_completion_tokens") && !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return forward_request("/v1/completions", modified);
}

json MlxServer::responses(const json& request) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "mlx-engine")
    );
}

} // namespace backends
} // namespace lemon
