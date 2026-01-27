#include "lemon/backends/fastflowlm_server.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <shellapi.h>
#include "../utils/wmi_helper.h"
#pragma comment(lib, "wbemuuid.lib")
#endif

// URL to direct users to for driver updates
static const std::string DRIVER_INSTALL_URL = "https://lemonade-server.ai/driver_install";

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
    
    // Reset upgrade tracking
    flm_was_upgraded_ = false;
    
    // Check NPU driver version first
    if (!check_npu_driver_version()) {
        throw std::runtime_error("NPU driver version check failed - please update your driver");
    }
    
    try {
        // Install FLM if needed (uses version from backend_versions.json)
        // Returns true if FLM was installed or upgraded
        flm_was_upgraded_ = install_flm_if_needed();
        
        // Verify flm is now available
        std::string flm_path = get_flm_path();
        if (flm_path.empty()) {
            throw std::runtime_error("FLM installation failed - not found in PATH");
        }
        
        std::cout << "[FastFlowLM] FLM ready at: " << flm_path << std::endl;
        
    } catch (const std::exception& e) {
        // Fallback: show manual installation instructions
        std::string required_version = get_flm_required_version();
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: FLM installation failed: " << e.what() << std::endl;
        std::cerr << std::string(70, '=') << std::endl;
        std::cerr << "\nPlease install FLM " << required_version << " manually:" << std::endl;
        std::cerr << "  https://github.com/FastFlowLM/FastFlowLM/releases/download/" 
                  << required_version << "/flm-setup.exe" << std::endl;
        std::cerr << "\nAfter installation, restart your terminal and try again." << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        throw;
    }
}

std::string FastFlowLMServer::download_model(const std::string& checkpoint,
                                            const std::string& mmproj,
                                            bool do_not_upgrade) {
    std::cout << "[FastFlowLM] Pulling model with FLM: " << checkpoint << std::endl;
    
    // Check NPU driver version before pulling models
    if (!check_npu_driver_version()) {
        throw std::runtime_error("NPU driver version check failed - please update your driver before pulling FLM models");
    }
    
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
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    std::cout << "[FastFlowLM] Loading model: " << model_name << std::endl;
    int ctx_size = options.get_option("ctx_size");

    // Note: checkpoint_ is set by Router via set_model_metadata() before load() is called
    // We use checkpoint_ (base class field) for FLM API calls
    
    // Check if model was downloaded before FLM upgrade (for invalidation detection)
    bool model_was_downloaded = model_manager_ && model_manager_->is_model_downloaded(model_name);
    
    // Install/check FLM
    install();
    
    // Check if FLM upgrade invalidated the model
    // This happens when a new FLM version requires models to be re-downloaded
    if (flm_was_upgraded_ && model_was_downloaded && model_manager_) {
        // Refresh and check if model is now marked as not downloaded
        model_manager_->refresh_flm_download_status();
        
        if (!model_manager_->is_model_downloaded(model_name)) {
            std::cout << "[FastFlowLM] Model '" << model_name 
                      << "' was invalidated by FLM upgrade" << std::endl;
            throw ModelInvalidatedException(model_name, 
                "FLM was upgraded and the model format has changed");
        }
    }
    
    // Download model if needed
    download_model(model_info.checkpoint, model_info.mmproj, do_not_upgrade);
    
    // Choose a port
    port_ = choose_port();
    
    // Get flm executable path
    std::string flm_path = get_flm_path();
    
    // Construct flm serve command
    // Bind to localhost only for security
    std::vector<std::string> args = {
        "serve",
        model_info.checkpoint,
        "--ctx-len", std::to_string(ctx_size),
        "--port", std::to_string(port_),
        "--host", "127.0.0.1"
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
    // FLM requires the checkpoint name in the request (e.g., "gemma3:4b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field
    
    return forward_request("/v1/chat/completions", modified_request);
}

json FastFlowLMServer::completion(const json& request) {
    // FLM requires the checkpoint name in the request (e.g., "lfm2:1.2b")
    // (whereas llama-server ignores the model name field)
    json modified_request = request;
    modified_request["model"] = checkpoint_;  // Use base class checkpoint field
    
    return forward_request("/v1/completions", modified_request);
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

void FastFlowLMServer::forward_streaming_request(const std::string& endpoint, 
                                                  const std::string& request_body,
                                                  httplib::DataSink& sink) {
    // FLM requires the checkpoint name in the model field (e.g., "gemma3:4b"),
    // not the Lemonade model name (e.g., "Gemma3-4b-it-FLM")
    try {
        json request = json::parse(request_body);
        request["model"] = checkpoint_;  // Use base class checkpoint field
        std::string modified_body = request.dump();
        
        // Call base class with modified request
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink);
    } catch (const json::exception& e) {
        // If JSON parsing fails, forward original request
        WrappedServer::forward_streaming_request(endpoint, request_body, sink);
    }
}

std::string FastFlowLMServer::get_flm_path() {
    // Use shared utility function to find flm executable
    // (find_flm_executable refreshes PATH from registry on Windows)
    std::string flm_path = utils::find_flm_executable();
    
    if (!flm_path.empty()) {
        std::cout << "[FastFlowLM] Found flm at: " << flm_path << std::endl;
    } else {
        std::cerr << "[FastFlowLM] flm not found in PATH" << std::endl;
    }
    
    return flm_path;
}

std::string FastFlowLMServer::get_flm_required_version() {
    // Get required FLM version from backend_versions.json
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");
    
    try {
        json config = utils::JsonUtils::load_from_file(config_path);
        
        if (!config.contains("flm") || !config["flm"].is_object()) {
            std::cerr << "[FastFlowLM] backend_versions.json is missing 'flm' section" << std::endl;
            return "v0.9.23";  // Fallback default
        }
        
        const auto& flm_config = config["flm"];
        
        if (!flm_config.contains("version") || !flm_config["version"].is_string()) {
            std::cerr << "[FastFlowLM] backend_versions.json is missing 'flm.version'" << std::endl;
            return "v0.9.23";  // Fallback default
        }
        
        return flm_config["version"].get<std::string>();
        
    } catch (const std::exception& e) {
        std::cerr << "[FastFlowLM] Error reading backend_versions.json: " << e.what() << std::endl;
        return "v0.9.23";  // Fallback default
    }
}

std::string FastFlowLMServer::get_min_npu_driver_version() {
    // Get minimum NPU driver version from backend_versions.json
    std::string config_path = utils::get_resource_path("resources/backend_versions.json");
    
    try {
        json config = utils::JsonUtils::load_from_file(config_path);
        
        if (!config.contains("flm") || !config["flm"].is_object()) {
            return "32.0.203.311";  // Fallback default
        }
        
        const auto& flm_config = config["flm"];
        
        if (!flm_config.contains("min_npu_driver") || !flm_config["min_npu_driver"].is_string()) {
            return "32.0.203.311";  // Fallback default
        }
        
        return flm_config["min_npu_driver"].get<std::string>();
        
    } catch (const std::exception& e) {
        std::cerr << "[FastFlowLM] Error reading backend_versions.json: " << e.what() << std::endl;
        return "32.0.203.311";  // Fallback default
    }
}

std::string FastFlowLMServer::get_flm_installed_version() {
    // Return cached version if available
    if (!cached_installed_version_.empty()) {
        return cached_installed_version_;
    }
    
    // Check if flm is installed
    std::string flm_path = get_flm_path();
    if (flm_path.empty()) {
        return "";
    }
    
    try {
        // Run flm --version command using the full path (not relying on PATH)
        std::string command = "\"" + flm_path + "\" --version 2>&1";
#ifdef _WIN32
        FILE* pipe = _popen(command.c_str(), "r");
#else
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (!pipe) {
            return "";
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
        
        // Parse output like "FLM v0.9.23" - look for "FLM v" specifically
        // to avoid matching 'v' in other text (like error messages)
        size_t pos = output.find("FLM v");
        if (pos != std::string::npos) {
            std::string version_str = output.substr(pos + 4);  // Skip "FLM "
            // Remove trailing whitespace and newlines
            size_t end = version_str.find_first_of(" \n\r\t");
            if (end != std::string::npos) {
                version_str = version_str.substr(0, end);
            }
            cached_installed_version_ = version_str;
            return cached_installed_version_;
        }
        
        return "";
        
    } catch (const std::exception& e) {
        return "";
    }
}

void FastFlowLMServer::invalidate_version_cache() {
    cached_installed_version_.clear();
}

std::string FastFlowLMServer::get_npu_driver_version() {
#ifdef _WIN32
    // Use WMI to query NPU driver version
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "";
    }
    
    std::string version;
    // Query for "NPU Compute Accelerator Device"
    std::wstring query = L"SELECT DriverVersion FROM Win32_PnPSignedDriver WHERE DeviceName LIKE '%NPU Compute Accelerator Device%'";
    
    wmi.query(query, [&version](IWbemClassObject* pObj) {
        if (version.empty()) {  // Only get first result
            version = wmi::get_property_string(pObj, L"DriverVersion");
        }
    });
    
    return version;
#else
    // NPU driver check is Windows-only
    return "0.0.0.0";
#endif
}

bool FastFlowLMServer::check_npu_driver_version() {
    std::string version = get_npu_driver_version();
    std::string min_version = get_min_npu_driver_version();
    
    if (version.empty()) {
        std::cout << "[FastFlowLM] NPU Driver Version: Unknown (Could not detect)" << std::endl;
        return true;  // Assume OK if we can't detect, to not block users with unusual setups
    }
    
    std::cout << "[FastFlowLM] NPU Driver Version: " << version << std::endl;
    
    // Parse and compare versions
    auto parse_version = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.')) {
            try {
                parts.push_back(std::stoi(part));
            } catch (...) {
                parts.push_back(0);
            }
        }
        while (parts.size() < 4) parts.push_back(0);  // Ensure 4 parts for driver versions
        return parts;
    };
    
    std::vector<int> current = parse_version(version);
    std::vector<int> minimum = parse_version(min_version);
    
    // Pad to same length
    size_t max_size = (current.size() > minimum.size()) ? current.size() : minimum.size();
    current.resize(max_size, 0);
    minimum.resize(max_size, 0);
    
    // Check if current < minimum
    bool too_old = false;
    for (size_t i = 0; i < max_size; ++i) {
        if (current[i] < minimum[i]) {
            too_old = true;
            break;
        }
        if (current[i] > minimum[i]) {
            break;  // Current is newer, we're OK
        }
    }
    
    if (too_old) {
        std::cerr << "\n" << std::string(70, '=') << std::endl;
        std::cerr << "ERROR: NPU Driver Version is too old!" << std::endl;
        std::cerr << "Current: " << version << std::endl;
        std::cerr << "Minimum: " << min_version << std::endl;
        std::cerr << "Please update your NPU driver at: " << DRIVER_INSTALL_URL << std::endl;
        std::cerr << std::string(70, '=') << std::endl << std::endl;
        
#ifdef _WIN32
        // Open browser to driver install page
        ShellExecuteA(NULL, "open", DRIVER_INSTALL_URL.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
        return false;
    }
    
    return true;
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

bool FastFlowLMServer::install_flm_if_needed() {
    std::string required_version = get_flm_required_version();
    std::string current_version = get_flm_installed_version();
    
    // Normalize versions for comparison (remove 'v' prefix if present)
    auto normalize_version = [](const std::string& v) -> std::string {
        if (!v.empty() && v[0] == 'v') {
            return v.substr(1);
        }
        return v;
    };
    
    std::string required_normalized = normalize_version(required_version);
    std::string current_normalized = normalize_version(current_version);
    
    // Case 1: Already have required version or newer
    if (!current_normalized.empty() && compare_versions(current_normalized, required_normalized)) {
        std::cout << "[FastFlowLM] FLM " << current_version 
                  << " is installed (required: " << required_version << ")" << std::endl;
        return false;  // No upgrade performed
    }
    
    // Case 2: Need to install or upgrade
    bool is_upgrade = !current_version.empty();
    if (is_upgrade) {
        std::cout << "[FastFlowLM] Upgrading FLM " << current_version 
                  << " → " << required_version << "..." << std::endl;
    } else {
        std::cout << "[FastFlowLM] Installing FLM " << required_version 
                  << "..." << std::endl;
    }
    
    // Determine installer path
#ifdef _WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    std::string installer_path = std::string(temp_path) + "flm-setup.exe";
#else
    std::string installer_path = "/tmp/flm-setup";
#endif
    
    // Delete any existing installer file to avoid collisions
    // We must succeed here to prevent running a stale installer
    if (fs::exists(installer_path)) {
        std::cout << "[FastFlowLM] Removing existing installer at: " << installer_path << std::endl;
        try {
            fs::remove(installer_path);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Could not remove existing installer at " + installer_path + ": " + e.what() + 
                ". Please delete it manually and try again.");
        }
        
        // Verify it's actually gone
        if (fs::exists(installer_path)) {
            throw std::runtime_error(
                "Failed to remove existing installer at " + installer_path + 
                ". Please delete it manually and try again.");
        }
    }
    
    if (!download_flm_installer(installer_path)) {
        throw std::runtime_error("Failed to download FLM installer");
    }
    
    // Run installer (silent for upgrades, GUI for fresh installs)
    run_flm_installer(installer_path, is_upgrade);
    
    // Invalidate version cache to force re-checking after installation
    invalidate_version_cache();
    
    // Verify installation by calling flm --version again
    if (!verify_flm_installation(required_normalized)) {
        throw std::runtime_error("FLM installation verification failed");
    }
    
    // Cleanup installer
    try {
        fs::remove(installer_path);
    } catch (...) {
        // Ignore cleanup errors
    }
    
    std::cout << "[FastFlowLM] Successfully installed FLM " 
              << required_version << std::endl;
    
    // Refresh FLM model download status in the model cache
    // This ensures any pre-existing FLM models are now detected
    // (FLM upgrade may invalidate previously downloaded models)
    if (model_manager_) {
        std::cout << "[FastFlowLM] Refreshing FLM model download status..." << std::endl;
        model_manager_->refresh_flm_download_status();
    }
    
    return true;  // FLM was installed or upgraded
}

bool FastFlowLMServer::download_flm_installer(const std::string& output_path) {
    // Get required version and build download URL
    std::string version = get_flm_required_version();
    const std::string url = 
        "https://github.com/FastFlowLM/FastFlowLM/releases/download/" + version + "/flm-setup.exe";
    
    std::cout << "[FastFlowLM] Downloading FLM " << version << " installer..." << std::endl;
    std::cout << "[FastFlowLM] URL: " << url << std::endl;
    
    auto progress_callback = utils::create_throttled_progress_callback();
    
    auto result = utils::HttpClient::download_file(url, output_path, progress_callback);
    
    if (result.success) {
        std::cout << "\n[FastFlowLM] Downloaded installer to " 
                  << output_path << std::endl;
    } else {
        std::cerr << "[FastFlowLM ERROR] Failed to download installer: " 
                  << result.error_message << std::endl;
    }
    
    return result.success;
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
    
    // Add default FLM installation path if not already in PATH
    const std::string flm_dir = "C:\\Program Files\\flm";
    if (fs::exists(flm_dir)) {
        const char* current_path = getenv("PATH");
        std::string current_path_str = current_path ? current_path : "";
        if (current_path_str.find(flm_dir) == std::string::npos) {
            _putenv(("PATH=" + flm_dir + ";" + current_path_str).c_str());
        }
    }
#endif
}

bool FastFlowLMServer::verify_flm_installation(const std::string& expected_version, int max_retries) {
    std::cout << "[FastFlowLM] Verifying installation..." << std::endl;
    
    std::this_thread::sleep_for(std::chrono::seconds(2)); // Initial wait
    
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        refresh_environment_path();
        
        // Invalidate cache to force fresh version check
        invalidate_version_cache();
        
        std::string current = get_flm_installed_version();
        
        // Normalize for comparison (remove 'v' prefix)
        std::string current_normalized = current;
        if (!current_normalized.empty() && current_normalized[0] == 'v') {
            current_normalized = current_normalized.substr(1);
        }
        
        if (!current_normalized.empty() && compare_versions(current_normalized, expected_version)) {
            std::cout << "[FastFlowLM] Verification successful: FLM " 
                      << current << std::endl;
            return true;
        }
        
        if (attempt < max_retries - 1) {
            std::cout << "[FastFlowLM] FLM not yet available (got: '" << current 
                      << "'), retrying... (" << (attempt + 1) << "/" << max_retries << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    
    std::cerr << "[FastFlowLM ERROR] FLM installation completed but 'flm' "
              << "is not available in PATH or version check failed" << std::endl;
    std::cerr << "Expected version: " << expected_version << std::endl;
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

