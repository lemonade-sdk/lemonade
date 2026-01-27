#include "lemon/backends/ryzenaiserver.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
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
using namespace lemon::utils;

namespace lemon {

#ifdef _WIN32
static const std::string RYZENAI_EXE_NAME = "ryzenai-server.exe";
#else
static const std::string RYZENAI_EXE_NAME = "ryzenai-server";
#endif

// Helper to load ryzenai-server version from configuration file
static std::string get_ryzenai_server_version() {
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");
    
    try {
        json config = utils::JsonUtils::load_from_file(config_path);
        
        if (!config.contains("ryzenai-server") || !config["ryzenai-server"].is_string()) {
            throw std::runtime_error("backend_versions.json is missing 'ryzenai-server' version");
        }
        
        std::string version = config["ryzenai-server"].get<std::string>();
        std::cout << "[RyzenAI-Server] Using version from config: " << version << std::endl;
        return version;
        
    } catch (const std::exception& e) {
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: Failed to load ryzenai-server version from configuration" << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nConfig file: " << config_path << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "\nThe backend_versions.json file is required and must contain valid" << std::endl;
        std::cerr << "version information for ryzenai-server." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw;
    }
}

// Helper to get the install directory for ryzenai-server
static std::string get_install_directory() {
    return (fs::path(utils::get_downloaded_bin_dir()) / "ryzenai-server").string();
}

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
    std::string install_dir;
    std::string version_file;

    // Check for custom binary via environment variable first
    std::string exe_path = find_external_ryzenai_server();
    bool needs_install = exe_path.empty();
    
    // Get expected version from config file
    std::string expected_version = get_ryzenai_server_version();
    
    if (needs_install) {
        install_dir = get_install_directory();
        version_file = (fs::path(install_dir) / "version.txt").string();
    
        // Check if already installed with correct version
        exe_path = find_executable_in_install_dir(install_dir);
        needs_install = exe_path.empty();
        
        if (!needs_install && fs::exists(version_file)) {
            std::string installed_version;
            
            // Read version info in a separate scope to ensure files are closed
            {
                std::ifstream vf(version_file);
                std::getline(vf, installed_version);
            }  // Files are closed here when ifstream objects go out of scope
            
            if (installed_version != expected_version) {
                std::cout << "[RyzenAI-Server] Upgrading from " << installed_version 
                        << " to " << expected_version << std::endl;
                needs_install = true;
                fs::remove_all(install_dir);
            }
        }
    }

    if (needs_install) {
        std::cout << "[RyzenAI-Server] Installing ryzenai-server (version: " << expected_version << ")" << std::endl;
        
        // Download and install ryzenai-server
        download_and_install(expected_version);
    } else {
        std::cout << "[RyzenAI-Server] Found ryzenai-server at: " << exe_path << std::endl;
    }
}

bool RyzenAIServer::is_available() {
    std::string exe_path = find_external_ryzenai_server();
    if (!exe_path.empty()) {
        return true;
    }
    
    std::string install_dir = get_install_directory();
    exe_path = find_executable_in_install_dir(install_dir);
    return !exe_path.empty();
}

std::string RyzenAIServer::find_external_ryzenai_server() {
    const char* ryzenai_bin_env = std::getenv("LEMONADE_RYZENAI_SERVER_BIN");
    if (!ryzenai_bin_env) {
        return "";
    }

    std::string ryzenai_bin = std::string(ryzenai_bin_env);
    
    return fs::exists(ryzenai_bin) ? ryzenai_bin : "";
}

std::string RyzenAIServer::find_executable_in_install_dir(const std::string& install_dir) {
    fs::path exe_path = fs::path(install_dir) / RYZENAI_EXE_NAME;
    if (fs::exists(exe_path)) {
        return fs::absolute(exe_path).string();
    }
    return "";
}

std::string RyzenAIServer::get_ryzenai_server_path() {
    // 1. Check for custom binary via environment variable
    std::string exe_path = find_external_ryzenai_server();
    if (!exe_path.empty()) {
        return exe_path;
    }
    
    // 2. Check in install directory (where download_and_install() places it)
    std::string install_dir = get_install_directory();
    exe_path = find_executable_in_install_dir(install_dir);
    
    if (!exe_path.empty()) {
        return exe_path;
    }
    
    // If not found, throw error with helpful message
    throw std::runtime_error("ryzenai-server not found in install directory: " + install_dir + 
                           "\nThis may indicate a failed installation or corrupted download.");
}

void RyzenAIServer::download_and_install(const std::string& version) {
    std::cout << "[RyzenAI-Server] Downloading ryzenai-server " << version << "..." << std::endl;
    
    // Download from GitHub release (from standalone ryzenai-server repo)
    std::string repo = "lemonade-sdk/ryzenai-server";
    std::string filename = "ryzenai-server.zip";
    std::string url = "https://github.com/" + repo + "/releases/download/" + version + "/" + filename;
    
    // Install to user cache directory
    fs::path install_dir = get_install_directory();
    std::string zip_path = (fs::path(utils::get_downloaded_bin_dir()) / filename).string();
    
    std::cout << "[RyzenAI-Server] Downloading from: " << url << std::endl;
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
        std::cerr << "[RyzenAI-Server ERROR]   - Version " << version << " has not been released yet" << std::endl;
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
    if (!backends::BackendUtils::extract_archive(zip_path, install_dir.string(), "RyzenAI-Server")) {
        // Clean up corrupted files
        fs::remove(zip_path);
        fs::remove_all(install_dir);
        throw std::runtime_error("Failed to extract ryzenai-server archive");
    }
    
    // Verify extraction succeeded by finding the executable
    std::string exe_path = find_executable_in_install_dir(install_dir.string());
    if (exe_path.empty()) {
        std::cerr << "[RyzenAI-Server ERROR] Extraction completed but executable not found in: " << install_dir << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] This usually indicates a corrupted download or unexpected archive structure." << std::endl;
        std::cerr << "[RyzenAI-Server ERROR] Cleaning up..." << std::endl;
        // Clean up corrupted files
        fs::remove(zip_path);
        fs::remove_all(install_dir);
        throw std::runtime_error("Extraction failed: executable not found. Downloaded file may be corrupted.");
    }
    
    std::cout << "[RyzenAI-Server] Executable verified at: " << exe_path << std::endl;
    
    // Save version info
    std::string version_file = (install_dir / "version.txt").string();
    std::ofstream vf(version_file);
    vf << version;
    vf.close();
    
#ifndef _WIN32
    // Make executable on Linux/macOS
    chmod(exe_path.c_str(), 0755);
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

