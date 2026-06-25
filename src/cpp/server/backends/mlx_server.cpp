// Required additions to backend_versions.json:
//   "mlx": { "cpu": "26aad7e" }
// Required additions to server_models.json (example):
//   "mlx-community/Llama-3.2-1B-Instruct-4bit": { "recipe": "mlx", "suggested": true }

#include "lemon/backends/mlx_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

InstallParams MLXServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    // MLX server is a system package (installed via pip/brew/manual build).
    // No auto-download from GitHub releases.
    return {};
}

MLXServer::MLXServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("mlx-server", log_level, model_manager, backend_manager) {
}

MLXServer::~MLXServer() {
    unload();
}

void MLXServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    LOG(INFO, "MLX") << "Loading model: " << model_name << std::endl;

    // MLX uses a local model directory path.
    // The checkpoint field in server_models.json is the path on disk.
    std::string model_path = model_info.checkpoint();
    if (model_path.empty()) {
        model_path = model_info.resolved_path();
    }
    if (model_path.empty()) {
        throw std::runtime_error("Model path not found for: " + model_name);
    }

    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model path does not exist: " + model_path);
    }

    LOG(DEBUG, "MLX") << "Using model path: " << model_path << std::endl;

    // Choose port
    port_ = choose_port();

    // Get executable path (mlx-server must be in PATH)
    // Using "system" backend to signal system-package install
    std::string executable = BackendUtils::get_backend_binary_path(SPEC, "system");

    // Build command line arguments
    // mlx-server <model_path> --port <PORT> --host 127.0.0.1
    std::vector<std::string> args;
    args.push_back(model_path);
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    args.push_back("--host");
    args.push_back("127.0.0.1");

    LOG(INFO, "MLX") << "Starting mlx-server on port " << port_ << "..." << std::endl;

    // Start process
    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = ProcessManager::start_process(executable, args, "", inherit_output, true);

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        throw std::runtime_error("mlx-server failed to start");
    }

    LOG(DEBUG, "MLX") << "Model loaded on port " << port_ << std::endl;
}

void MLXServer::unload() {
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

json MLXServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json MLXServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json MLXServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

} // namespace backends
} // namespace lemon
