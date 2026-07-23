#include "lemon/backends/amdgpuserver.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

InstallParams AMDGPUServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    // No public GitHub release; the binary is installed locally into the cache
    // (bin/amdgpu-server/gpu/). These values are only consulted if a download is
    // triggered, which does not happen while the installed version.txt matches
    // the pin in backend_versions.json.
    return {"lemonade-sdk/amdgpu-server", "amdgpu-server.zip"};
}

AMDGPUServer::AMDGPUServer(const std::string& model_name, bool debug, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("AMDGPU-Server", debug ? "debug" : "info", model_manager, backend_manager),
      model_name_(model_name),
      is_loaded_(false) {
}

AMDGPUServer::~AMDGPUServer() {
    if (is_loaded_) {
        try {
            unload();
        } catch (...) {
            // Suppress exceptions in destructor
        }
    }
}

bool AMDGPUServer::is_available() {
    try {
        return !backends::BackendUtils::get_backend_binary_path(SPEC, "gpu").empty();
    } catch (...) {
        return false;
    }
}

void AMDGPUServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    LOG(DEBUG, "AMDGPU") << "Loading model: " << model_name << std::endl;
    int ctx_size = options.get_option("ctx_size");

    // Install/check amdgpu-server (resolves the locally-installed binary; will
    // only attempt a download if version.txt is missing/mismatched).
    backend_manager_->install_backend("amdgpu-llm", "gpu");

    // Get the path to amdgpu-server
    std::string amdgpu_server_path = backends::BackendUtils::get_backend_binary_path(SPEC, "gpu");
    if (amdgpu_server_path.empty()) {
        throw std::runtime_error("AMDGPU-Server executable not found even after installation attempt");
    }

    LOG(DEBUG, "AMDGPU") << "Found amdgpu-server at: " << amdgpu_server_path << std::endl;

    // Model path should have been set via set_model_path() before calling load()
    if (model_path_.empty()) {
        throw std::runtime_error("Model path is required for AMDGPU-Server. Call set_model_path() before load()");
    }

    if (!fs::exists(model_path_)) {
        throw std::runtime_error("Model path does not exist: " + model_path_);
    }

    model_name_ = model_name;

    LOG(DEBUG, "AMDGPU") << "Model path: " << model_path_ << std::endl;

    // Find available port
    port_ = choose_port();

    // Build command line arguments
    std::vector<std::string> args = {
        "-m", model_path_,
        "--port", std::to_string(port_),
        "--ctx-size", std::to_string(ctx_size)
    };

    if (is_debug()) {
        args.push_back("--verbose");
    }

    // Log the full command line
    LOG(DEBUG, "AMDGPU") << "Starting: \"" << amdgpu_server_path << "\"";
    for (const auto& arg : args) {
        LOG(DEBUG, "AMDGPU") << " \"" << arg << "\"";
    }
    LOG(DEBUG, "AMDGPU") << std::endl;

    // Start the process (filter health check spam)
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        amdgpu_server_path,
        args,
        "",
        is_debug(),
        true
    );
    set_process_handle(started_handle);

    if (!utils::ProcessManager::is_running(started_handle)) {
        throw std::runtime_error("Failed to start amdgpu-server process");
    }

    LOG(DEBUG, "ProcessManager") << "Process started successfully, PID: "
                << started_handle.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("AMDGPU-Server failed to start (check logs for details)");
    }

    is_loaded_ = true;
    LOG(INFO, "AMDGPU") << "Model loaded on port " << get_backend_port() << std::endl;
}

void AMDGPUServer::unload() {
    stop_backend_watchdog();
    LOG(DEBUG, "AMDGPU") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        utils::ProcessManager::stop_process(handle);
    }

    is_loaded_ = false;
    model_path_.clear();
}

json AMDGPUServer::chat_completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("AMDGPU-Server");
    }
    return forward_request("/v1/chat/completions", request);
}

json AMDGPUServer::completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("AMDGPU-Server");
    }
    return forward_request("/v1/completions", request);
}

json AMDGPUServer::responses(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("AMDGPU-Server");
    }
    return forward_request("/v1/responses", request);
}

} // namespace lemon
