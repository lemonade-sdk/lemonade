#include "lemon/backends/ryzenaiserver.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <map>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>  // For chmod on Linux/macOS
#endif

namespace fs = std::filesystem;

namespace lemon {

RyzenAIServer::RyzenAIServer(const std::string& model_name, bool debug, ModelManager* model_manager)
    : WrappedServer("RyzenAI-Server", debug ? "debug" : "info", model_manager), 
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
    // Check if already installed
    std::string path = get_ryzenai_server_path();
    if (!path.empty()) {
        std::cout << "[RyzenAI-Server] Found existing installation at: " << path << std::endl;
        return;
    }
    
    std::cout << "[RyzenAI-Server] ryzenai-server not found, downloading..." << std::endl;
    
    // Download and install ryzenai-server
    download_and_install();
}

// Helper to get the install directory for ryzenai-server
static std::string get_install_directory() {
    return (fs::path(utils::get_downloaded_bin_dir()) / "ryzenai-server").string();
}

#ifdef _WIN32
static const std::string RYZENAI_EXE_NAME = "ryzenai-server.exe";
#else
static const std::string RYZENAI_EXE_NAME = "ryzenai-server";
#endif

bool RyzenAIServer::is_available() {
    return !get_ryzenai_server_path().empty();
}

std::string RyzenAIServer::get_ryzenai_server_path() {
    // 1. Check in source tree location (for developers)
    std::string relative_path = utils::get_resource_path("../../../ryzenai-server/build/bin/Release/" + RYZENAI_EXE_NAME);
    if (fs::exists(relative_path)) {
        return fs::absolute(relative_path).string();
    }
    
    // 2. Check in user cache directory (where download_and_install() places it)
    fs::path install_path = fs::path(get_install_directory()) / RYZENAI_EXE_NAME;
    if (fs::exists(install_path)) {
        return fs::absolute(install_path).string();
    }
    
    return ""; // Not found
}

// Helper function to extract ZIP files
static bool extract_zip(const std::string& zip_path, const std::string& dest_dir) {
#ifdef _WIN32
    std::cout << "[RyzenAI-Server] Extracting ZIP to " << dest_dir << std::endl;
    
    // Use PowerShell to extract with error handling
    std::string command = "powershell -Command \"try { Expand-Archive -Path '" + 
                         zip_path + "' -DestinationPath '" + dest_dir + 
                         "' -Force -ErrorAction Stop; exit 0 } catch { Write-Error $_.Exception.Message; exit 1 }\"";
    
    int result = system(command.c_str());
    if (result != 0) {
        std::cerr << "[RyzenAI-Server] PowerShell extraction failed with code: " << result << std::endl;
        return false;
    }
    return true;
#else
    std::cout << "[RyzenAI-Server] Extracting ZIP to " << dest_dir << std::endl;
    std::string command = "unzip -o \"" + zip_path + "\" -d \"" + dest_dir + "\"";
    int result = system(command.c_str());
    return result == 0;
#endif
}

void RyzenAIServer::download_and_install() {
    std::cout << "[RyzenAI-Server] Downloading ryzenai-server..." << std::endl;
    
    // Download from latest GitHub release
    std::string repo = "lemonade-sdk/lemonade";
    std::string filename = "ryzenai-server.zip";
    std::string url = "https://github.com/" + repo + "/releases/latest/download/" + filename;
    
    // Install to user cache directory
    fs::path install_dir = get_install_directory();
    std::string zip_path = (fs::path(utils::get_downloaded_bin_dir()) / filename).string();
    
    std::cout << "[RyzenAI-Server] Downloading from latest GitHub release..." << std::endl;
    std::cout << "[RyzenAI-Server] Installing to: " << install_dir.string() << std::endl;
    
    // Download the ZIP file with throttled progress updates (once per second)
    // No authentication needed for public releases
    std::map<std::string, std::string> headers;
    auto download_result = utils::HttpClient::download_file(
        url, 
        zip_path,
        utils::create_throttled_progress_callback(),
        headers
    );
    
    if (!download_result.success) {
        std::cerr << "\n[RyzenAI-Server ERROR] Failed to download ryzenai-server: " << download_result.error_message << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] Possible causes:" << std::endl;
        std::cerr << "[RyzenAI-Server ERROR]   - No internet connection or GitHub is down" << std::endl;
        std::cerr << "[RyzenAI-Server ERROR]   - No release has been published yet" << std::endl;
        std::cerr << "[RyzenAI-Server ERROR]   - The release does not contain " << filename << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] Check releases at: https://github.com/" << repo << "/releases" << std::endl;
        throw std::runtime_error("Failed to download ryzenai-server from release");
    }
    
    std::cout << "[RyzenAI-Server] Download complete!" << std::endl;
    
    // Verify the downloaded file exists and is valid
    if (!fs::exists(zip_path)) {
        throw std::runtime_error("Downloaded ZIP file does not exist: " + zip_path);
    }
    
    std::uintmax_t file_size = fs::file_size(zip_path);
    std::cout << "[RyzenAI-Server] Downloaded ZIP file size: " << (file_size / 1024 / 1024) << " MB" << std::endl;
    
    const std::uintmax_t MIN_ZIP_SIZE = 1024 * 1024;  // 1 MB
    if (file_size < MIN_ZIP_SIZE) {
        std::cerr << "[RyzenAI-Server ERROR] Downloaded file is too small (" << file_size << " bytes)" << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] This usually indicates a failed or incomplete download." << std::endl;
        fs::remove(zip_path);
        throw std::runtime_error("Downloaded file is too small (< 1 MB), likely corrupted or incomplete");
    }
    
    // Create install directory
    fs::create_directories(install_dir);
    
    // Extract ZIP
    if (!extract_zip(zip_path, install_dir.string())) {
        // Clean up corrupted files
        fs::remove(zip_path);
        fs::remove_all(install_dir);
        throw std::runtime_error("Failed to extract ryzenai-server archive");
    }
    
    // Debug: List what was extracted
    std::cout << "[RyzenAI-Server DEBUG] Contents of extracted directory:" << std::endl;
    try {
        int file_count = 0;
        for (const auto& entry : fs::directory_iterator(install_dir)) {
            std::cout << "  - " << entry.path().filename().string() << std::endl;
            file_count++;
            if (file_count > 20) {
                std::cout << "  ... (and more files)" << std::endl;
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[RyzenAI-Server ERROR] Failed to list directory: " << e.what() << std::endl;
    }
    
    // Verify extraction succeeded by checking if executable exists
#ifdef _WIN32
    std::string exe_name = "ryzenai-server.exe";
#else
    std::string exe_name = "ryzenai-server";
#endif
    
    fs::path exe_path_check = install_dir / exe_name;
    std::cout << "[RyzenAI-Server DEBUG] Looking for executable at: " << exe_path_check << std::endl;
    
    if (!fs::exists(exe_path_check)) {
        std::cerr << "[RyzenAI-Server ERROR] Extraction completed but executable not found at: " 
                  << exe_path_check << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] This usually indicates the ZIP structure is different than expected." << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] Check the extracted files above for the correct location." << std::endl;
        // Don't clean up yet - let user inspect the directory
        throw std::runtime_error("Extraction failed: executable not found in expected location.");
    }
    
    std::cout << "[RyzenAI-Server] Executable verified at: " << exe_path_check << std::endl;
    
#ifndef _WIN32
    // Make executable on Linux/macOS
    chmod(exe_path_check.c_str(), 0755);
#endif
    
    // Delete ZIP file
    fs::remove(zip_path);
    
    std::cout << "[RyzenAI-Server] Installation complete!" << std::endl;
}

std::string RyzenAIServer::download_model(const std::string& checkpoint,
                                         const std::string& mmproj,
                                         bool do_not_upgrade) {
    // RyzenAI-Server uses ONNX models downloaded via Hugging Face
    // The model is expected to already be downloaded in ONNX format
    std::cout << "[RyzenAI-Server] Note: RyzenAI-Server requires pre-downloaded ONNX models" << std::endl;
    std::cout << "[RyzenAI-Server] Expected checkpoint format: repository/model-name" << std::endl;
    std::cout << "[RyzenAI-Server] Model will be loaded from Hugging Face cache" << std::endl;
    
    return checkpoint;
}

std::string RyzenAIServer::determine_execution_mode(const std::string& model_path,
                                                   const std::string& backend) {
    // Map backend to execution mode
    if (backend == "npu" || backend == "oga-npu") {
        return "npu";
    } else if (backend == "hybrid" || backend == "oga-hybrid") {
        return "hybrid";
    } else if (backend == "cpu" || backend == "oga-cpu") {
        return "cpu";
    } else {
        // "auto" will let ryzenai-server decide
        return "auto";
    }
}

void RyzenAIServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    std::cout << "[RyzenAI-Server] Loading model: " << model_name << std::endl;
    int ctx_size = options.get_option("ctx_size");

    // Install/check RyzenAI-Server (will download if not found)
    install();
    
    // Get the path to ryzenai-server
    std::string ryzenai_server_path = get_ryzenai_server_path();
    if (ryzenai_server_path.empty()) {
        // This shouldn't happen after install(), but check anyway
        throw std::runtime_error("RyzenAI-Server executable not found even after installation attempt");
    }
    
    std::cout << "[RyzenAI-Server] Found ryzenai-server at: " << ryzenai_server_path << std::endl;
    
    // Model path should have been set via set_model_path() before calling load()
    if (model_path_.empty()) {
        throw std::runtime_error("Model path is required for RyzenAI-Server. Call set_model_path() before load()");
    }
    
    if (!fs::exists(model_path_)) {
        throw std::runtime_error("Model path does not exist: " + model_path_);
    }
    
    model_name_ = model_name;
    
    // execution_mode_ should have been set via set_execution_mode() before calling load()
    if (execution_mode_.empty()) {
        execution_mode_ = "auto";
    }
    
    std::cout << "[RyzenAI-Server] Model path: " << model_path_ << std::endl;
    std::cout << "[RyzenAI-Server] Execution mode: " << execution_mode_ << std::endl;
    
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
    
    // Log the full command line
    std::cout << "[RyzenAI-Server] Starting: \"" << ryzenai_server_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // Start the process (filter health check spam)
    process_handle_ = utils::ProcessManager::start_process(
        ryzenai_server_path,
        args,
        "",
        is_debug(),
        true
    );
    
    if (!utils::ProcessManager::is_running(process_handle_)) {
        throw std::runtime_error("Failed to start ryzenai-server process");
    }
    
    std::cout << "[ProcessManager] Process started successfully, PID: " 
              << process_handle_.pid << std::endl;
    
    // Wait for server to be ready
    if (!wait_for_ready()) {
        throw std::runtime_error("RyzenAI-Server failed to start (check logs for details)");
    }
    
    is_loaded_ = true;
    std::cout << "[RyzenAI-Server] Model loaded on port " << port_ << std::endl;
}

void RyzenAIServer::unload() {
    if (!is_loaded_) {
        return;
    }
    
    std::cout << "[RyzenAI-Server] Unloading model..." << std::endl;
    
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
    }
    
    is_loaded_ = false;
    port_ = 0;
    model_path_.clear();
}

json RyzenAIServer::chat_completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }
    
    // Forward to /v1/chat/completions endpoint
    return forward_request("/v1/chat/completions", request);
}

json RyzenAIServer::completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }
    
    // Forward to /v1/completions endpoint
    return forward_request("/v1/completions", request);
}

json RyzenAIServer::responses(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }
    
    // Forward to /v1/responses endpoint
    return forward_request("/v1/responses", request);
}

} // namespace lemon

