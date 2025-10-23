#include "lemon/backends/ryzenaiserver.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace lemon {

RyzenAIServer::RyzenAIServer(const std::string& model_name, int port, bool debug)
    : WrappedServer("RyzenAI-Serve", debug ? "debug" : "info"), 
      model_name_(model_name),
      execution_mode_("auto"),
      is_loaded_(false) {
}

RyzenAIServer::~RyzenAIServer() {
    if (is_loaded_) {
        try {
            unload();
        } catch (...) {
            // Suppress exceptions in destructor
        }
    }
}

void RyzenAIServer::install(const std::string& backend) {
    std::cout << "[RyzenAI-Serve] Installation Instructions:" << std::endl;
    std::cout << "[RyzenAI-Serve] RyzenAI-Serve must be built from source." << std::endl;
    std::cout << "[RyzenAI-Serve] Please follow the build instructions at:" << std::endl;
    std::cout << "[RyzenAI-Serve] https://github.com/amd/ryzenai-serve" << std::endl;
    std::cout << "[RyzenAI-Serve] After building, ensure ryzenai-serve.exe is in your PATH" << std::endl;
    std::cout << "[RyzenAI-Serve] or place it in the lemonade installation directory." << std::endl;
    
    if (!is_available()) {
        throw std::runtime_error("RyzenAI-Serve not found. Please install it first.");
    }
}

bool RyzenAIServer::is_available() {
    std::string path = get_ryzenai_serve_path();
    return !path.empty();
}

std::string RyzenAIServer::get_ryzenai_serve_path() {
    // Check in PATH
#ifdef _WIN32
    std::string exe_name = "ryzenai-serve.exe";
#else
    std::string exe_name = "ryzenai-serve";
#endif
    
    // Check if executable exists in PATH
    // Simple check: try to find it using 'where' on Windows or 'which' on Unix
#ifdef _WIN32
    std::string check_cmd = "where " + exe_name + " >nul 2>&1";
#else
    std::string check_cmd = "which " + exe_name + " >/dev/null 2>&1";
#endif
    
    if (system(check_cmd.c_str()) == 0) {
        return exe_name;
    }
    
    // Check in common locations relative to lemonade executable
    // From executable location to src/ryzenai-serve/build/bin/Release
    std::string relative_path = utils::get_resource_path("../../../ryzenai-serve/build/bin/Release/" + exe_name);
    if (fs::exists(relative_path)) {
        return fs::absolute(relative_path).string();
    }
    
    return ""; // Not found
}

std::string RyzenAIServer::download_model(const std::string& checkpoint,
                                         const std::string& mmproj,
                                         bool do_not_upgrade) {
    // RyzenAI-Serve uses ONNX models downloaded via Hugging Face
    // The model is expected to already be downloaded in ONNX format
    std::cout << "[RyzenAI-Serve] Note: RyzenAI-Serve requires pre-downloaded ONNX models" << std::endl;
    std::cout << "[RyzenAI-Serve] Expected checkpoint format: repository/model-name" << std::endl;
    std::cout << "[RyzenAI-Serve] Model will be loaded from Hugging Face cache" << std::endl;
    
    return checkpoint;
}

std::string RyzenAIServer::determine_execution_mode(const std::string& model_path,
                                                   const std::string& backend) {
    // Map backend to execution mode
    if (backend == "npu") {
        return "npu";
    } else if (backend == "hybrid" || backend == "oga-hybrid") {
        return "hybrid";
    } else {
        // "auto" will let ryzenai-serve decide
        return "auto";
    }
}

void RyzenAIServer::load(const std::string& model_name,
                        const std::string& checkpoint,
                        const std::string& mmproj,
                        int ctx_size,
                        bool do_not_upgrade,
                        const std::vector<std::string>& labels) {
    std::cout << "[RyzenAI-Serve] Loading model: " << model_name << std::endl;
    
    // Check if RyzenAI-Serve is available
    std::string ryzenai_serve_path = get_ryzenai_serve_path();
    if (ryzenai_serve_path.empty()) {
        std::cerr << "[RyzenAI-Serve ERROR] ryzenai-serve.exe not found in PATH" << std::endl;
        std::cerr << "[RyzenAI-Serve ERROR] Please build from source or ensure it's in your PATH" << std::endl;
        throw std::runtime_error("RyzenAI-Serve executable not found");
    }
    
    std::cout << "[RyzenAI-Serve] Found ryzenai-serve at: " << ryzenai_serve_path << std::endl;
    
    // Model path should have been set via set_model_path() before calling load()
    if (model_path_.empty()) {
        throw std::runtime_error("Model path is required for RyzenAI-Serve. Call set_model_path() before load()");
    }
    
    if (!fs::exists(model_path_)) {
        throw std::runtime_error("Model path does not exist: " + model_path_);
    }
    
    model_name_ = model_name;
    
    // execution_mode_ should have been set via set_execution_mode() before calling load()
    if (execution_mode_.empty()) {
        execution_mode_ = "auto";
    }
    
    std::cout << "[RyzenAI-Serve] Model path: " << model_path_ << std::endl;
    std::cout << "[RyzenAI-Serve] Execution mode: " << execution_mode_ << std::endl;
    
    // Find available port
    port_ = choose_port();
    
    // Build command line arguments
    std::vector<std::string> args = {
        "-m", model_path_,
        "--port", std::to_string(port_),
        "--mode", execution_mode_,
        "--ctx-size", std::to_string(ctx_size)
    };
    
    if (is_debug()) {
        args.push_back("--verbose");
    }
    
    std::cout << "[RyzenAI-Serve] Starting ryzenai-serve..." << std::endl;
    
    // Start the process
    process_handle_ = utils::ProcessManager::start_process(
        ryzenai_serve_path,
        args,
        "",
        is_debug()
    );
    
    if (!utils::ProcessManager::is_running(process_handle_)) {
        throw std::runtime_error("Failed to start ryzenai-serve process");
    }
    
    std::cout << "[ProcessManager] Process started successfully, PID: " 
              << process_handle_.pid << std::endl;
    
    // Wait for server to be ready
    wait_for_ready();
    
    is_loaded_ = true;
    std::cout << "[RyzenAI-Serve] Model loaded on port " << port_ << std::endl;
}

void RyzenAIServer::unload() {
    if (!is_loaded_) {
        return;
    }
    
    std::cout << "[RyzenAI-Serve] Unloading model..." << std::endl;
    
    if (process_handle_.handle) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
    }
    
    is_loaded_ = false;
    port_ = 0;
    model_path_.clear();
}

json RyzenAIServer::chat_completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Serve");
    }
    
    // Forward to /v1/chat/completions endpoint
    return forward_request("/v1/chat/completions", request);
}

json RyzenAIServer::completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Serve");
    }
    
    // Forward to /v1/completions endpoint
    return forward_request("/v1/completions", request);
}

void RyzenAIServer::parse_telemetry(const std::string& line) {
    // RyzenAI-Serve outputs telemetry in its responses, not in stdout
    // So this method is a no-op for this backend
    // Telemetry will be parsed from the JSON response itself
}

} // namespace lemon

