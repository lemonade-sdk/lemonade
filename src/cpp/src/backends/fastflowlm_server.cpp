#include "lemon/backends/fastflowlm_server.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

FastFlowLMServer::FastFlowLMServer(const std::string& log_level)
    : WrappedServer("FastFlowLM", log_level) {
}

FastFlowLMServer::~FastFlowLMServer() {
    unload();
}

void FastFlowLMServer::install(const std::string& backend) {
    std::cout << "[FastFlowLM] Checking FLM installation..." << std::endl;
    
    // Check if flm is available in PATH
    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: FastFlowLM (FLM) is not installed!" << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nFLM is required to run NPU-accelerated models." << std::endl;
        std::cerr << "\nPlease download and install FLM from:" << std::endl;
        std::cerr << "  https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe" << std::endl;
        std::cerr << "\nAfter installation, restart your terminal and try again." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw std::runtime_error("FLM not installed. Please install FLM and try again.");
    }
    
    std::cout << "[FastFlowLM] Found FLM at: " << flm_path << std::endl;
}

std::string FastFlowLMServer::download_model(const std::string& checkpoint,
                                            const std::string& mmproj,
                                            bool do_not_upgrade) {
    std::cout << "[FastFlowLM] Pulling model with FLM: " << checkpoint << std::endl;
    
    // Use flm pull command to download the model
    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        throw std::runtime_error("FLM not found");
    }
    
    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }
    
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // Run flm pull command (with debug output if enabled)
    auto handle = utils::ProcessManager::start_process(flm_path, args, "", is_debug());
    
    // Wait for download to complete
    if (!utils::ProcessManager::is_running(handle)) {
        int exit_code = utils::ProcessManager::get_exit_code(handle);
        std::cerr << "[FastFlowLM ERROR] FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed");
    }
    
    // Wait for process to complete
    int timeout_seconds = 300; // 5 minutes
    std::cout << "[FastFlowLM] Waiting for model download to complete..." << std::endl;
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        if (!utils::ProcessManager::is_running(handle)) {
            int exit_code = utils::ProcessManager::get_exit_code(handle);
            if (exit_code != 0) {
                std::cerr << "[FastFlowLM ERROR] FLM pull failed with exit code: " << exit_code << std::endl;
                throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            std::cout << "[FastFlowLM] Still downloading... (" << (i/10) << "s elapsed)" << std::endl;
        }
    }
    
    std::cout << "[FastFlowLM] Model pull completed successfully" << std::endl;
    return checkpoint;
}

void FastFlowLMServer::load(const std::string& model_name,
                           const std::string& checkpoint,
                           const std::string& mmproj,
                           int ctx_size,
                           bool do_not_upgrade,
                           const std::vector<std::string>& labels) {
    std::cout << "[FastFlowLM] Loading model: " << model_name << std::endl;
    
    // Store model name for later use
    model_name_ = checkpoint;
    
    // Install/check FLM
    install();
    
    // Download model if needed
    download_model(checkpoint, mmproj, do_not_upgrade);
    
    // Choose a port
    port_ = choose_port();
    std::cout << "flm-server will use port: " << port_ << std::endl;
    
    // Get flm executable path
    std::string flm_path = get_flm_path();
    
    // Construct flm serve command
    std::vector<std::string> args = {
        "serve",
        checkpoint,
        "--ctx-len", std::to_string(ctx_size),
        "--port", std::to_string(port_)
    };
    
    std::cout << "[FastFlowLM] Starting flm-server..." << std::endl;
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // Start the flm serve process
    process_handle_ = utils::ProcessManager::start_process(flm_path, args, "", is_debug());
    std::cout << "[ProcessManager] Process started successfully" << std::endl;
    
    // Wait for flm-server to be ready
    bool ready = wait_for_ready();
    if (!ready) {
        utils::ProcessManager::stop_process(process_handle_);
        throw std::runtime_error("flm-server failed to start");
    }
    
    is_loaded_ = true;
    std::cout << "[FastFlowLM] Model loaded on port " << port_ << std::endl;
}

void FastFlowLMServer::unload() {
    std::cout << "[FastFlowLM] Unloading model..." << std::endl;
    if (is_loaded_ && process_handle_.handle) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        model_name_.clear();
        is_loaded_ = false;
    }
}

bool FastFlowLMServer::wait_for_ready() {
    // FLM doesn't have a health endpoint, so we use /api/tags to check if it's up
    std::string tags_url = get_base_url() + "/api/tags";
    
    std::cout << "Waiting for " + server_name_ + " to be ready..." << std::endl;
    
    const int max_attempts = 60;  // 60 seconds timeout
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            std::cerr << "[ERROR] " << server_name_ << " process has terminated!" << std::endl;
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[ERROR] Process exit code: " << exit_code << std::endl;
            std::cerr << "\nTroubleshooting tips:" << std::endl;
            std::cerr << "  1. Check if FLM is installed correctly: flm --version" << std::endl;
            std::cerr << "  2. Try running: flm serve <model> --ctx-len 8192 --port 8001" << std::endl;
            std::cerr << "  3. Check NPU drivers are installed (Windows only)" << std::endl;
            return false;
        }
        
        // Try to reach the /api/tags endpoint (sleep 1 second between attempts)
        if (utils::HttpClient::is_reachable(tags_url, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return true;
        }
        
        // No need to sleep here - is_reachable already sleeps 1 second
    }
    
    std::cerr << "[ERROR] " << server_name_ << " failed to start within " 
              << max_attempts << " seconds" << std::endl;
    return false;
}

json FastFlowLMServer::chat_completion(const json& request) {
    // FLM requires the correct checkpoint name in the request
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = model_name_;  // Use the checkpoint (e.g., "qwen3:0.6b")
    
    return forward_request("/v1/chat/completions", modified_request);
}

json FastFlowLMServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json FastFlowLMServer::embeddings(const json& request) {
    return forward_request("/v1/embeddings", request);
}

json FastFlowLMServer::reranking(const json& request) {
    return forward_request("/v1/rerank", request);
}

json FastFlowLMServer::responses(const json& request) {
    // Responses API is not supported for FLM backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "flm")
    );
}

void FastFlowLMServer::parse_telemetry(const std::string& line) {
    // FLM telemetry parsing can be added here if needed
    // For now, we'll rely on the response from the server
}

std::string FastFlowLMServer::get_flm_path() {
    // Check common locations for flm executable
#ifdef _WIN32
    // On Windows, check PATH
    const char* paths[] = {
        "flm.exe",
        "C:\\Program Files\\FastFlowLM\\flm.exe",
        "C:\\Program Files (x86)\\FastFlowLM\\flm.exe"
    };
    
    for (const auto& path : paths) {
        // Try to find in PATH
        std::string cmd = "where " + std::string(path) + " >nul 2>&1";
        if (system(cmd.c_str()) == 0) {
            // Found in PATH, return just the name
            if (std::string(path) == "flm.exe") {
                return "flm";
            }
            return path;
        }
        // Check if file exists at absolute path
        if (fs::exists(path)) {
            return path;
        }
    }
#else
    // On Linux/Mac, check PATH
    if (system("which flm >/dev/null 2>&1") == 0) {
        return "flm";
    }
#endif
    
    return ""; // Not found
}

bool FastFlowLMServer::check_npu_available() {
#ifdef _WIN32
    // Check for AMD NPU driver on Windows
    // This is a simplified check - in production you'd want more robust detection
    const char* npu_paths[] = {
        "C:\\Windows\\System32\\drivers\\amdxdna.sys",
        "C:\\Windows\\System32\\DriverStore\\FileRepository\\amdxdna.inf_amd64_*\\amdxdna.sys"
    };
    
    for (const auto& path : npu_paths) {
        if (fs::exists(path)) {
            return true;
        }
    }
#endif
    return false;
}

} // namespace backends
} // namespace lemon

