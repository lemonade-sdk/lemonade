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

FastFlowLMServer::FastFlowLMServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("FastFlowLM", log_level, model_manager) {
}

FastFlowLMServer::~FastFlowLMServer() {
    unload();
}

void FastFlowLMServer::install(const std::string& backend) {
    std::cout << "[FastFlowLM] Checking FLM installation..." << std::endl;
    
    try {
        // This will auto-install or auto-upgrade as needed
        install_or_upgrade_flm();
        
        // Verify flm is now available
        std::string flm_path = get_flm_path();
        if (flm_path.empty()) {
            throw std::runtime_error("FLM installation failed - not found in PATH");
        }
        
        std::cout << "[FastFlowLM] FLM ready at: " << flm_path << std::endl;
        
    } catch (const std::exception& e) {
        // Fallback: show manual installation instructions
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: FLM auto-installation failed: " << e.what() << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nPlease install FLM manually:" << std::endl;
        std::cerr << "  https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe" << std::endl;
        std::cerr << "\nAfter installation, restart your terminal and try again." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw;
    }
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
                           const ModelInfo& model_info,
                           int ctx_size,
                           bool do_not_upgrade) {
    std::cout << "[FastFlowLM] Loading model: " << model_name << std::endl;
    
    // Store model name for later use
    model_name_ = model_info.checkpoint;
    
    // Install/check FLM
    install();
    
    // Download model if needed
    download_model(model_info.checkpoint, model_info.mmproj, do_not_upgrade);
    
    // Choose a port
    port_ = choose_port();
    
    // Get flm executable path
    std::string flm_path = get_flm_path();
    
    // Construct flm serve command
    std::vector<std::string> args = {
        "serve",
        model_info.checkpoint,
        "--ctx-len", std::to_string(ctx_size),
        "--port", std::to_string(port_)
    };
    
    std::cout << "[FastFlowLM] Starting flm-server..." << std::endl;
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // Start the flm serve process (filter health check spam)
    process_handle_ = utils::ProcessManager::start_process(flm_path, args, "", is_debug(), true);
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
    
    const int max_attempts = 300;  // 5 minutes timeout (large models can take time to load)
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

std::string FastFlowLMServer::get_flm_latest_version() {
    // Get latest version from GitHub API
    const std::string url = "https://api.github.com/repos/FastFlowLM/FastFlowLM/tags";
    
    try {
        auto response = utils::HttpClient::get(url);
        if (response.status_code != 200) {
            std::cerr << "[FastFlowLM] Failed to fetch latest version (HTTP " 
                     << response.status_code << ")" << std::endl;
            return "";
        }
        
        // Parse JSON response
        auto tags = json::parse(response.body);
        if (!tags.is_array() || tags.empty()) {
            return "";
        }
        
        // Find first valid version tag
        for (const auto& tag : tags) {
            if (!tag.contains("name")) continue;
            
            std::string tag_name = tag["name"];
            std::string version_candidate = tag_name;
            
            // Remove 'v' prefix if present
            if (!version_candidate.empty() && version_candidate[0] == 'v') {
                version_candidate = version_candidate.substr(1);
            }
            
            // Validate it looks like a version (has digits and dots)
            bool valid = false;
            for (char c : version_candidate) {
                if (std::isdigit(c) || c == '.') {
                    valid = true;
                    break;
                }
            }
            
            if (valid) {
                return version_candidate;
            }
        }
        
        return "";
        
    } catch (const std::exception& e) {
        std::cerr << "[FastFlowLM] Error retrieving latest version: " 
                 << e.what() << std::endl;
        return "";
    }
}

std::pair<std::string, std::string> FastFlowLMServer::check_flm_version() {
    // Get latest version first
    std::string latest_version = get_flm_latest_version();
    
    // Check installed version
    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        return {"", latest_version};
    }
    
    try {
        // Run flm version command
        std::vector<std::string> args = {"version"};
        auto handle = utils::ProcessManager::start_process(flm_path, args, "", false);
        
        // Wait for command to complete (with timeout)
        for (int i = 0; i < 50; ++i) {  // 5 second timeout
            if (!utils::ProcessManager::is_running(handle)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        int exit_code = utils::ProcessManager::get_exit_code(handle);
        if (exit_code != 0) {
            return {"", latest_version};
        }
        
        // Parse version from output (e.g., "FLM v0.9.4")
        // Note: ProcessManager captures output, but we need to read it
        // For now, we'll assume if flm exists and runs, we can check version differently
        // Let's try a simpler approach using system() and capturing output
        
#ifdef _WIN32
        FILE* pipe = _popen("flm version 2>&1", "r");
#else
        FILE* pipe = popen("flm version 2>&1", "r");
#endif
        if (!pipe) {
            return {"", latest_version};
        }
        
        char buffer[256];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        
        // Parse output like "FLM v0.9.4"
        size_t pos = output.find("FLM v");
        if (pos != std::string::npos) {
            std::string version_str = output.substr(pos + 5);
            // Remove trailing whitespace and newlines
            version_str.erase(version_str.find_last_not_of(" \n\r\t") + 1);
            return {version_str, latest_version};
        }
        
        return {"", latest_version};
        
    } catch (const std::exception& e) {
        return {"", latest_version};
    }
}

bool FastFlowLMServer::compare_versions(const std::string& v1, const std::string& v2) {
    // Compare semantic versions: return true if v1 >= v2
    if (v1.empty() || v2.empty()) {
        return false;
    }
    
    auto parse_version = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::string part;
        for (char c : v) {
            if (c == '.') {
                if (!part.empty()) {
                    parts.push_back(std::stoi(part));
                    part.clear();
                }
            } else if (std::isdigit(c)) {
                part += c;
            }
        }
        if (!part.empty()) {
            parts.push_back(std::stoi(part));
        }
        return parts;
    };
    
    auto parts1 = parse_version(v1);
    auto parts2 = parse_version(v2);
    
    // Pad with zeros to make same length (avoid std::max due to Windows.h max macro)
    size_t max_len = parts1.size() > parts2.size() ? parts1.size() : parts2.size();
    while (parts1.size() < max_len) parts1.push_back(0);
    while (parts2.size() < max_len) parts2.push_back(0);
    
    // Compare each part
    for (size_t i = 0; i < max_len; ++i) {
        if (parts1[i] > parts2[i]) return true;
        if (parts1[i] < parts2[i]) return false;
    }
    
    return true; // Equal versions
}

void FastFlowLMServer::install_or_upgrade_flm() {
    auto version_info = check_flm_version();
    std::string current_version = version_info.first;
    std::string latest_version = version_info.second;
    
    // Case 1: Already up-to-date
    if (!current_version.empty() && !latest_version.empty() 
        && compare_versions(current_version, latest_version)) {
        std::cout << "[FastFlowLM] FLM v" << current_version 
                  << " is up to date (latest: v" << latest_version << ")" << std::endl;
        return;
    }
    
    // Case 2: Cannot determine latest version, continue with current
    if (!current_version.empty() && latest_version.empty()) {
        std::cout << "[FastFlowLM] Cannot check latest version, "
                  << "continuing with FLM v" << current_version << std::endl;
        return;
    }
    
    // Case 3: Upgrade needed or fresh install
    bool is_upgrade = !current_version.empty();
    if (is_upgrade) {
        std::cout << "[FastFlowLM] Upgrading FLM v" << current_version 
                  << " → v" << latest_version << "..." << std::endl;
    } else {
        std::cout << "[FastFlowLM] Installing FLM v" << latest_version 
                  << "..." << std::endl;
    }
    
    // Download installer
#ifdef _WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    std::string installer_path = std::string(temp_path) + "flm-setup.exe";
#else
    std::string installer_path = "/tmp/flm-setup";
#endif
    
    if (!download_flm_installer(installer_path)) {
        throw std::runtime_error("Failed to download FLM installer");
    }
    
    // Run installer (silent for upgrades, GUI for fresh installs)
    run_flm_installer(installer_path, is_upgrade);
    
    // Verify installation
    if (!verify_flm_installation(latest_version)) {
        throw std::runtime_error("FLM installation verification failed");
    }
    
    // Cleanup
    try {
        fs::remove(installer_path);
    } catch (...) {
        // Ignore cleanup errors
    }
    
    std::cout << "[FastFlowLM] Successfully installed FLM v" 
              << latest_version << std::endl;
}

bool FastFlowLMServer::download_flm_installer(const std::string& output_path) {
    const std::string url = 
        "https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe";
    
    std::cout << "[FastFlowLM] Downloading FLM installer..." << std::endl;
    
    // Use existing HttpClient::download_file() with progress callback
    auto progress_callback = utils::create_throttled_progress_callback();
    
    bool success = utils::HttpClient::download_file(url, output_path, progress_callback);
    
    if (success) {
        std::cout << "\n[FastFlowLM] Downloaded installer to " 
                  << output_path << std::endl;
    } else {
        std::cerr << "[FastFlowLM ERROR] Failed to download installer" << std::endl;
    }
    
    return success;
}

void FastFlowLMServer::run_flm_installer(const std::string& installer_path, bool silent) {
    std::vector<std::string> args;
    if (silent) {
        args.push_back("/VERYSILENT");
        std::cout << "[FastFlowLM] Running silent upgrade..." << std::endl;
    } else {
        std::cout << "[FastFlowLM] Launching installer GUI. "
                  << "Please complete the installation..." << std::endl;
    }
    
    // Launch installer and wait for completion
    auto handle = utils::ProcessManager::start_process(installer_path, args, "", false);
    
    std::cout << "[FastFlowLM] Waiting for installer to complete..." << std::endl;
    
    // Wait for installer to complete
    int timeout_seconds = 300; // 5 minutes
    for (int i = 0; i < timeout_seconds * 2; ++i) {
        if (!utils::ProcessManager::is_running(handle)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Print progress every 10 seconds
        if (!silent && i % 20 == 0 && i > 0) {
            std::cout << "[FastFlowLM] Still waiting... (" << (i/2) << "s elapsed)" << std::endl;
        }
    }
    
    int exit_code = utils::ProcessManager::get_exit_code(handle);
    if (exit_code != 0) {
        throw std::runtime_error(
            "FLM installer failed with exit code: " + std::to_string(exit_code));
    }
    
    std::cout << "[FastFlowLM] Installer completed successfully" << std::endl;
}

void FastFlowLMServer::refresh_environment_path() {
#ifdef _WIN32
    // Refresh PATH from Windows registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[32767];
        DWORD bufferSize = sizeof(buffer);
        if (RegQueryValueExA(hKey, "PATH", nullptr, nullptr, 
                            reinterpret_cast<LPBYTE>(buffer), &bufferSize) == ERROR_SUCCESS) {
            std::string new_path = buffer;
            // Append to existing PATH instead of replacing
            const char* current_path = getenv("PATH");
            if (current_path) {
                new_path = new_path + ";" + std::string(current_path);
            }
            _putenv(("PATH=" + new_path).c_str());
        }
        RegCloseKey(hKey);
    }
    
    // Also add common FLM installation paths
    std::vector<std::string> common_paths = {
        "C:\\Program Files\\FastFlowLM",
        "C:\\Program Files (x86)\\FastFlowLM"
    };
    
    // Add user-specific path
    char* local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data) {
        common_paths.push_back(std::string(local_app_data) + "\\FastFlowLM");
    }
    
    for (const auto& path : common_paths) {
        if (fs::exists(path)) {
            const char* current_path = getenv("PATH");
            std::string current_path_str = current_path ? current_path : "";
            if (current_path_str.find(path) == std::string::npos) {
                _putenv(("PATH=" + path + ";" + current_path_str).c_str());
            }
        }
    }
#endif
}

bool FastFlowLMServer::verify_flm_installation(const std::string& expected_version, int max_retries) {
    std::cout << "[FastFlowLM] Verifying installation..." << std::endl;
    
    std::this_thread::sleep_for(std::chrono::seconds(2)); // Initial wait
    
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        refresh_environment_path();
        
        auto version_info = check_flm_version();
        std::string current = version_info.first;
        if (!current.empty() && compare_versions(current, expected_version)) {
            std::cout << "[FastFlowLM] Verification successful: FLM v" 
                      << current << std::endl;
            return true;
        }
        
        if (attempt < max_retries - 1) {
            std::cout << "[FastFlowLM] FLM not yet available, retrying... ("
                      << (attempt + 1) << "/" << max_retries << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    
    std::cerr << "[FastFlowLM ERROR] FLM installation completed but 'flm' "
              << "is not available in PATH" << std::endl;
    std::cerr << "Please restart your terminal or add FLM to your PATH manually." << std::endl;
    return false;
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

