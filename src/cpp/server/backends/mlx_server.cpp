#include "lemon/backends/mlx_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/system_info.h"
#include "lemon/error_types.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

InstallParams MLXServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "system") {
        return params;
    }

    // MLX engine releases from stampby/lemon-mlx-engine
    params.repo = "stampby/lemon-mlx-engine";

    std::string bare_version = version;
    if (!bare_version.empty() && bare_version[0] == 'v') {
        bare_version = bare_version.substr(1);
    }

#ifdef __linux__
    params.filename = "lemon-mlx-engine-" + bare_version + "-linux-rocm.tar.gz";
#else
    throw std::runtime_error(
        "MLX ROCm backend is currently Linux-only. "
        "See: https://github.com/stampby/lemon-mlx-engine/releases/tag/" + version);
#endif

    return params;
}

MLXServer::MLXServer(const std::string& log_level, ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("MLX", log_level, model_manager, backend_manager) {
}

MLXServer::~MLXServer() {
    unload();
}

std::string MLXServer::download_model(const std::string& checkpoint, bool do_not_upgrade) {
    // MLX engine auto-downloads HuggingFace models on first request.
    // No explicit download step needed — the server handles it internally.
    LOG(INFO, "MLX") << "Model will be auto-downloaded on first request: " << checkpoint << std::endl;
    return checkpoint;
}

void MLXServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    LOG(INFO, "MLX") << "Loading model: " << model_name << std::endl;

    // Get MLX-specific options from RecipeOptions
    int ctx_size = options.get_option("ctx_size");
    std::string mlx_args = options.get_option("mlx_args");
    int max_tokens = options.get_option("mlx_max_tokens");

    std::cout << "[MLX] Options: ctx_size=" << ctx_size;
    if (!mlx_args.empty()) {
        std::cout << ", mlx_args=\"" << mlx_args << "\"";
    }
    std::cout << std::endl;

    // Check if MLX server binary is available (env var or local path first, then auto-install)
    std::string mlx_path = get_mlx_server_path();
    if (mlx_path.empty()) {
        // Only try auto-install if no local binary found
        backend_manager_->install_backend(SPEC.recipe, "rocm");
        mlx_path = get_mlx_server_path();
        if (mlx_path.empty()) {
            throw std::runtime_error("MLX engine server binary not found. "
                "Set LEMONADE_MLX_ROCM_BIN environment variable to the path of your mlx-engine server binary.");
        }
    }

    // Choose a port
    port_ = choose_port();

    std::vector<std::string> args = {
        model_info.checkpoint(),
        "--port", std::to_string(port_),
        "--host", "127.0.0.1",
        "--max-tokens", std::to_string(max_tokens > 0 ? max_tokens : 4096)
    };

    // Add context size if specified
    if (ctx_size > 0) {
        args.push_back("--ctx-size");
        args.push_back(std::to_string(ctx_size));
    }

    // Parse and append custom mlx_args if provided
    if (!mlx_args.empty()) {
        std::istringstream iss(mlx_args);
        std::string token;
        while (iss >> token) {
            args.push_back(token);
        }
    }

    LOG(INFO, "MLX") << "Starting mlx-engine server..." << std::endl;
    LOG(INFO, "ProcessManager") << "Starting process: \"" << mlx_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    // Set ROCm environment variables for gfx1151 (Strix Halo)
    // The process inherits the parent environment; these ensure correct GPU targeting
    process_handle_ = utils::ProcessManager::start_process(mlx_path, args, "", is_debug(), true);
    LOG(INFO, "ProcessManager") << "Process started successfully" << std::endl;

    // Wait for mlx-engine server to be ready
    bool ready = wait_for_ready();
    if (!ready) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        throw std::runtime_error("MLX engine server failed to start");
    }

    is_loaded_ = true;
    LOG(INFO, "MLX") << "Model loaded on port " << port_ << std::endl;
}

void MLXServer::unload() {
    LOG(INFO, "MLX") << "Unloading model..." << std::endl;
    if (is_loaded_ && process_handle_.pid != 0) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        is_loaded_ = false;
    }
}

bool MLXServer::wait_for_ready() {
    // MLX engine has a /health endpoint
    std::string health_url = get_base_url() + "/health";

    LOG(INFO, "MLX") << "Waiting for MLX engine to be ready..." << std::endl;

    const int max_attempts = 300;  // 5 minutes timeout (model download can take time)
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            LOG(ERROR, "MLX") << "MLX engine process has terminated!" << std::endl;
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            LOG(ERROR, "MLX") << "Process exit code: " << exit_code << std::endl;
            LOG(ERROR, "MLX") << "Troubleshooting tips:" << std::endl;
            LOG(ERROR, "MLX") << "  1. Check ROCm is installed: rocm-smi" << std::endl;
            LOG(ERROR, "MLX") << "  2. Check GPU is detected: rocminfo" << std::endl;
            LOG(ERROR, "MLX") << "  3. Try running: mlx-engine server --port 8080" << std::endl;
            return false;
        }

        // Try to reach the /health endpoint
        if (utils::HttpClient::is_reachable(health_url, 1)) {
            LOG(INFO, "MLX") << "MLX engine is ready!" << std::endl;
            return true;
        }

        // Sleep 1 second between attempts
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG(ERROR, "MLX") << "MLX engine failed to start within "
              << max_attempts << " seconds" << std::endl;
    return false;
}

json MLXServer::chat_completion(const json& request) {
    // MLX engine needs the HuggingFace checkpoint name, not the Lemonade model name
    json modified_request = request;
    modified_request["model"] = checkpoint_;
    return forward_request("/v1/chat/completions", modified_request);
}

json MLXServer::completion(const json& request) {
    // MLX engine needs the HuggingFace checkpoint name, not the Lemonade model name
    json modified_request = request;
    modified_request["model"] = checkpoint_;
    return forward_request("/v1/completions", modified_request);
}

json MLXServer::responses(const json& request) {
    // Responses API is not supported for MLX backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "mlx")
    );
}

void MLXServer::forward_streaming_request(const std::string& endpoint,
                                           const std::string& request_body,
                                           httplib::DataSink& sink,
                                           bool sse,
                                           long timeout_seconds) {
    // MLX engine needs the HuggingFace checkpoint name in the model field
    try {
        json request = json::parse(request_body);
        request["model"] = checkpoint_;
        std::string modified_body = request.dump();
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse, timeout_seconds);
    } catch (const json::exception& e) {
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds);
    }
}

std::string MLXServer::get_mlx_server_path() {
    // Check environment variable first: LEMONADE_MLX_ROCM_BIN
    std::string env_path = BackendUtils::find_external_backend_binary("mlx", "rocm");
    if (!env_path.empty()) {
        LOG(INFO, "MLX") << "Using MLX server from env: " << env_path << std::endl;
        return env_path;
    }

    // Check installed backend binary
    try {
        std::string path = BackendUtils::get_backend_binary_path(SPEC, "rocm");
        LOG(INFO, "MLX") << "Found MLX server at: " << path << std::endl;
        return path;
    } catch (const std::exception& e) {
        LOG(DEBUG, "MLX") << "MLX server not in install dir: " << e.what() << std::endl;
    }

    // Fallback: search common locations
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : "/tmp";
    std::vector<std::string> search_paths = {
        home + "/mlx-engine-bin/server",
        home + "/.local/bin/mlx-engine-server",
        "/usr/local/bin/mlx-engine-server",
        "/usr/bin/mlx-engine-server"
    };

    for (const auto& path : search_paths) {
        if (fs::exists(path) && fs::is_regular_file(path)) {
            LOG(INFO, "MLX") << "Found MLX server at: " << path << std::endl;
            return path;
        }
    }

    LOG(ERROR, "MLX") << "MLX engine server not found" << std::endl;
    return "";
}

} // namespace backends
} // namespace lemon
