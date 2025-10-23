#include <lemon/model_manager.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

ModelManager::ModelManager() {
    server_models_ = load_server_models();
    user_models_ = load_user_models();
}

std::string ModelManager::get_cache_dir() {
    // Check environment variable first
    const char* cache_env = std::getenv("LEMONADE_CACHE_DIR");
    if (cache_env) {
        return std::string(cache_env);
    }
    
    // Default cache directory
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.cache\\lemonade";
    }
    return "C:\\.cache\\lemonade";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/lemonade";
    }
    return "/tmp/lemonade";
#endif
}

std::string ModelManager::get_user_models_file() {
    return get_cache_dir() + "/user_models.json";
}

json ModelManager::load_server_models() {
    try {
        // Load from resources directory (relative to executable)
        std::string models_path = get_resource_path("resources/server_models.json");
        return JsonUtils::load_from_file(models_path);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to load server_models.json: " << e.what() << std::endl;
        std::cerr << "This is a critical file required for the application to run." << std::endl;
        std::cerr << "Executable directory: " << get_executable_dir() << std::endl;
        throw std::runtime_error("Failed to load server_models.json");
    }
}

json ModelManager::load_user_models() {
    std::string user_models_path = get_user_models_file();
    
    if (!fs::exists(user_models_path)) {
        return json::object();
    }
    
    try {
        return JsonUtils::load_from_file(user_models_path);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not load user_models.json: " << e.what() << std::endl;
        return json::object();
    }
}

void ModelManager::save_user_models(const json& user_models) {
    std::string user_models_path = get_user_models_file();
    
    // Ensure directory exists
    fs::path dir = fs::path(user_models_path).parent_path();
    fs::create_directories(dir);
    
    JsonUtils::save_to_file(user_models, user_models_path);
}

std::map<std::string, ModelInfo> ModelManager::get_supported_models() {
    std::map<std::string, ModelInfo> models;
    
    // Load server models
    for (auto& [key, value] : server_models_.items()) {
        ModelInfo info;
        info.model_name = key;
        info.checkpoint = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = JsonUtils::get_or_default<bool>(value, "suggested", false);
        info.mmproj = JsonUtils::get_or_default<std::string>(value, "mmproj", "");
        
        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }
        
        models[key] = info;
    }
    
    // Load user models with "user." prefix
    for (auto& [key, value] : user_models_.items()) {
        ModelInfo info;
        info.model_name = "user." + key;
        info.checkpoint = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = false;
        info.mmproj = JsonUtils::get_or_default<std::string>(value, "mmproj", "");
        
        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }
        
        models[info.model_name] = info;
    }
    
    // Filter by backend availability before returning
    return filter_models_by_backend(models);
}

std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    auto all_models = get_supported_models(); // Already filtered by backend
    std::map<std::string, ModelInfo> downloaded;
    
    // OPTIMIZATION: List HF cache directory once instead of checking each model
    std::string hf_cache;
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        hf_cache = std::string(hf_home_env) + "/hub";
    } else {
#ifdef _WIN32
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            hf_cache = std::string(home) + "/.cache/huggingface/hub";
        }
#endif
    }
    
    // Build a set of available model directories by listing the HF cache once
    std::unordered_set<std::string> available_hf_models;
    if (!hf_cache.empty() && fs::exists(hf_cache)) {
        try {
            for (const auto& entry : fs::directory_iterator(hf_cache)) {
                if (entry.is_directory()) {
                    std::string dir_name = entry.path().filename().string();
                    // Only consider directories that start with "models--"
                    if (dir_name.find("models--") == 0) {
                        available_hf_models.insert(dir_name);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[ModelManager] Warning: Could not list HF cache: " << e.what() << std::endl;
        }
    }
    
    // Get FLM models once
    auto flm_models = get_flm_installed_models();
    std::unordered_set<std::string> available_flm_models(flm_models.begin(), flm_models.end());
    
    // Now filter models using in-memory lookups (no filesystem calls per model!)
    for (const auto& [name, info] : all_models) {
        bool is_available = false;
        
        if (info.recipe == "flm") {
            // Check FLM set
            is_available = available_flm_models.count(info.checkpoint) > 0;
        } else {
            // Convert checkpoint to cache directory name
            std::string checkpoint = info.checkpoint;
            size_t colon_pos = checkpoint.find(':');
            if (colon_pos != std::string::npos) {
                checkpoint = checkpoint.substr(0, colon_pos);
            }
            
            std::string cache_dir_name = "models--";
            for (char c : checkpoint) {
                if (c == '/') {
                    cache_dir_name += "--";
                } else {
                    cache_dir_name += c;
                }
            }
            
            // Check HF set
            is_available = available_hf_models.count(cache_dir_name) > 0;
        }
        
        if (is_available) {
            downloaded[name] = info;
        }
    }
    
    return downloaded;
}

// Helper function to check if NPU is available
// Matches Python behavior: on Windows, assume available (FLM will fail at runtime if not compatible)
// This allows showing FLM models on Windows systems - the actual compatibility check happens when loading
static bool is_npu_available() {
#ifdef _WIN32
    // Check if user explicitly disabled NPU check
    const char* skip_check = std::getenv("RYZENAI_SKIP_PROCESSOR_CHECK");
    if (skip_check && (std::string(skip_check) == "1" || 
                       std::string(skip_check) == "true" || 
                       std::string(skip_check) == "yes")) {
        return true;
    }
    
    // On Windows, we assume NPU is available for filtering purposes
    // The real compatibility check happens at runtime when FLM tries to use the NPU
    // This matches the Python implementation which only raises exceptions for non-Windows platforms
    return true;
#else
    // Non-Windows platforms don't support FLM
    return false;
#endif
}

// Helper function to check if FLM is available
static bool is_flm_available() {
#ifdef _WIN32
    return system("where flm > nul 2>&1") == 0;
#else
    return system("which flm > /dev/null 2>&1") == 0;
#endif
}

static bool is_ryzenai_available() {
#ifdef _WIN32
    // Check if ryzenai-serve.exe is in PATH or in the ryzenai-serve build directory
    if (system("where ryzenai-serve > nul 2>&1") == 0) {
        return true;
    }
    
    // Check in relative path (from executable location to src/ryzenai-serve/build/bin/Release)
    std::string relative_path = get_resource_path("../../../ryzenai-serve/build/bin/Release/ryzenai-serve.exe");
    if (std::filesystem::exists(relative_path)) {
        return true;
    }
    
    return false;
#else
    return system("which ryzenai-serve > /dev/null 2>&1") == 0;
#endif
}

std::map<std::string, ModelInfo> ModelManager::filter_models_by_backend(
    const std::map<std::string, ModelInfo>& models) {
    
    std::map<std::string, ModelInfo> filtered;
    
    // Detect platform
#ifdef __APPLE__
    bool is_macos = true;
#else
    bool is_macos = false;
#endif
    
    // Check backend availability
    bool flm_exe_available = is_flm_available();
    bool npu_hw_available = is_npu_available();
    bool flm_available = flm_exe_available && npu_hw_available;
    bool ryzenai_available = is_ryzenai_available();
    
    // Debug output (only shown once during startup)
    static bool debug_printed = false;
    if (!debug_printed) {
        std::cout << "[ModelManager] Backend availability:" << std::endl;
        std::cout << "  - FLM executable: " << (flm_exe_available ? "Yes" : "No") << std::endl;
        std::cout << "  - NPU hardware: " << (npu_hw_available ? "Yes" : "No") << std::endl;
        std::cout << "  - FLM support: " << (flm_available ? "Enabled" : "Disabled") << std::endl;
        std::cout << "  - RyzenAI-Serve: " << (ryzenai_available ? "Yes" : "No") << std::endl;
        debug_printed = true;
    }
    
    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        
        // Filter FLM models based on availability
        if (recipe == "flm") {
            if (!flm_available) {
                continue;
            }
        }
        
        // Filter RyzenAI (OGA) models based on availability
        if (recipe == "oga-npu" || recipe == "oga-hybrid") {
            if (!ryzenai_available) {
                continue;
            }
        }
        
        // Filter out other OGA models (not yet implemented)
        if (recipe == "oga-cpu" || recipe == "oga-igpu") {
            continue;
        }
        
        // On macOS, only show llamacpp models
        if (is_macos && recipe != "llamacpp") {
            continue;
        }
        
        // Model passes all filters
        filtered[name] = info;
    }
    
    return filtered;
}

void ModelManager::register_user_model(const std::string& model_name,
                                      const std::string& checkpoint,
                                      const std::string& recipe,
                                      bool reasoning,
                                      bool vision,
                                      const std::string& mmproj) {
    
    // Remove "user." prefix if present
    std::string clean_name = model_name;
    if (clean_name.substr(0, 5) == "user.") {
        clean_name = clean_name.substr(5);
    }
    
    json model_entry;
    model_entry["checkpoint"] = checkpoint;
    model_entry["recipe"] = recipe;
    
    std::vector<std::string> labels;
    if (reasoning) {
        labels.push_back("reasoning");
    }
    if (vision) {
        labels.push_back("vision");
    }
    if (!labels.empty()) {
        model_entry["labels"] = labels;
    }
    
    if (!mmproj.empty()) {
        model_entry["mmproj"] = mmproj;
    }
    
    json updated_user_models = user_models_;
    updated_user_models[clean_name] = model_entry;
    
    save_user_models(updated_user_models);
    user_models_ = updated_user_models;
}

// Helper function to get FLM installed models by calling 'flm list'
std::vector<std::string> ModelManager::get_flm_installed_models() {
    std::vector<std::string> installed_models;
    
#ifdef _WIN32
    std::string command = "where flm > nul 2>&1";
#else
    std::string command = "which flm > /dev/null 2>&1";
#endif
    
    // Check if flm is available
    if (system(command.c_str()) != 0) {
        return installed_models; // FLM not installed
    }
    
    // Run 'flm list' to get installed models
#ifdef _WIN32
    FILE* pipe = _popen("flm list", "r");
#else
    FILE* pipe = popen("flm list", "r");
#endif
    
    if (!pipe) {
        return installed_models;
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
    
    // Parse output - look for lines starting with "- " and ending with " ✅"
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.find("- ") == 0) {
            // Remove "- " prefix
            std::string model_info = line.substr(2);
            
            // Check if model is installed (ends with ✅)
            // Note: ✅ is UTF-8, so we need to check for the byte sequence
            if (model_info.size() >= 4 && 
                (model_info.substr(model_info.size() - 4) == " \xE2\x9C\x85" || 
                 model_info.find(" \xE2\x9C\x85") != std::string::npos)) {
                // Remove the checkmark and trim
                size_t checkmark_pos = model_info.find(" \xE2\x9C\x85");
                if (checkmark_pos != std::string::npos) {
                    std::string checkpoint = model_info.substr(0, checkmark_pos);
                    checkpoint.erase(0, checkpoint.find_first_not_of(" \t"));
                    checkpoint.erase(checkpoint.find_last_not_of(" \t") + 1);
                    installed_models.push_back(checkpoint);
                }
            }
        }
    }
    
    return installed_models;
}

bool ModelManager::is_model_downloaded(const std::string& model_name) {
    // Call the optimized version with empty FLM cache (will fetch on demand)
    static std::vector<std::string> empty_cache;
    return is_model_downloaded(model_name, nullptr);
}

bool ModelManager::is_model_downloaded(const std::string& model_name, 
                                       const std::vector<std::string>* flm_cache) {
    auto info = get_model_info(model_name);
    
    // Check FLM models separately
    if (info.recipe == "flm") {
        // Use cached FLM list if provided, otherwise fetch it
        std::vector<std::string> flm_models;
        if (flm_cache) {
            flm_models = *flm_cache;
        } else {
            flm_models = get_flm_installed_models();
        }
        
        for (const auto& installed : flm_models) {
            if (installed == info.checkpoint) {
                return true;
            }
        }
        return false;
    }
    
    // Get Hugging Face cache directory (not Lemonade cache!)
    std::string hf_cache;
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        hf_cache = std::string(hf_home_env) + "/hub";
    } else {
#ifdef _WIN32
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
        } else {
            return false;
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            hf_cache = std::string(home) + "/.cache/huggingface/hub";
        } else {
            return false;
        }
#endif
    }
    
    // Parse checkpoint to get repo ID
    std::string checkpoint = info.checkpoint;
    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        checkpoint = checkpoint.substr(0, colon_pos);
    }
    
    // Convert checkpoint to cache directory name
    // Format: models--<org>--<model>
    std::string cache_dir_name = "models--";
    for (char c : checkpoint) {
        if (c == '/') {
            cache_dir_name += "--";
        } else {
            cache_dir_name += c;
        }
    }
    
    std::string model_cache_path = hf_cache + "/" + cache_dir_name;
    
    // OPTIMIZATION: Just check if directory exists instead of recursive scan
    // This is much faster and sufficient to determine if a model is downloaded
    return fs::exists(model_cache_path) && fs::is_directory(model_cache_path);
}

void ModelManager::download_model(const std::string& model_name,
                                 const std::string& checkpoint,
                                 const std::string& recipe,
                                 bool reasoning,
                                 bool vision,
                                 const std::string& mmproj,
                                 bool do_not_upgrade) {
    
    std::string actual_checkpoint = checkpoint;
    std::string actual_recipe = recipe;
    
    // If checkpoint not provided, look up from registry
    if (actual_checkpoint.empty()) {
        auto info = get_model_info(model_name);
        actual_checkpoint = info.checkpoint;
        actual_recipe = info.recipe;
    }
    
    // Parse checkpoint
    std::string repo_id = actual_checkpoint;
    std::string variant = "";
    
    size_t colon_pos = actual_checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = actual_checkpoint.substr(0, colon_pos);
        variant = actual_checkpoint.substr(colon_pos + 1);
    }
    
    std::cout << "Downloading model: " << repo_id;
    if (!variant.empty()) {
        std::cout << " (variant: " << variant << ")";
    }
    std::cout << std::endl;
    
    // Check if offline mode
    const char* offline_env = std::getenv("LEMONADE_OFFLINE");
    if (offline_env && std::string(offline_env) == "1") {
        std::cout << "Offline mode enabled, skipping download" << std::endl;
        return;
    }
    
    // If already downloaded and do_not_upgrade, skip
    if (do_not_upgrade && is_model_downloaded(model_name)) {
        std::cout << "Model already downloaded, skipping" << std::endl;
        return;
    }
    
    // Use FLM pull for FLM models, otherwise download from HuggingFace
    if (actual_recipe == "flm") {
        download_from_flm(actual_checkpoint, do_not_upgrade);
    } else {
        download_from_huggingface(repo_id, variant);
    }
    
    // Register if needed
    if (model_name.substr(0, 5) == "user." || !checkpoint.empty()) {
        register_user_model(model_name, actual_checkpoint, actual_recipe, 
                          reasoning, vision, mmproj);
    }
}

void ModelManager::download_from_huggingface(const std::string& repo_id,
                                            const std::string& variant) {
    // Get Hugging Face cache directory
    std::string hf_cache;
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        hf_cache = std::string(hf_home_env) + "/hub";
    } else {
#ifdef _WIN32
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
        } else {
            throw std::runtime_error("Cannot determine HF cache directory");
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            hf_cache = std::string(home) + "/.cache/huggingface/hub";
        } else {
            throw std::runtime_error("Cannot determine HF cache directory");
        }
#endif
    }
    
    // Create cache directory structure
    fs::create_directories(hf_cache);
    
    std::string cache_dir_name = "models--";
    for (char c : repo_id) {
        if (c == '/') {
            cache_dir_name += "--";
        } else {
            cache_dir_name += c;
        }
    }
    
    std::string model_cache_path = hf_cache + "/" + cache_dir_name;
    fs::create_directories(model_cache_path);
    
    // Get HF token if available
    std::map<std::string, std::string> headers;
    const char* hf_token = std::getenv("HF_TOKEN");
    if (hf_token) {
        headers["Authorization"] = "Bearer " + std::string(hf_token);
    }
    
    // List files in repository
    std::string api_url = "https://huggingface.co/api/models/" + repo_id;
    
    try {
        auto response = HttpClient::get(api_url, headers);
        
        if (response.status_code == 200) {
            auto model_info = JsonUtils::parse(response.body);
            
            // Download files based on recipe/variant
            if (model_info.contains("siblings") && model_info["siblings"].is_array()) {
                for (const auto& file : model_info["siblings"]) {
                    std::string filename = file["rfilename"].get<std::string>();
                    
                    // Filter files based on variant for GGUF models
                    if (!variant.empty()) {
                        // Only download files matching the variant
                        if (filename.find(variant) == std::string::npos && 
                            filename != "config.json" &&
                            filename != "tokenizer.json" &&
                            filename != "tokenizer_config.json") {
                            continue;
                        }
                    }
                    
                    // Download file
                    std::string file_url = "https://huggingface.co/" + repo_id + 
                                         "/resolve/main/" + filename;
                    std::string output_path = model_cache_path + "/snapshots/main/" + filename;
                    
                    // Create parent directory
                    fs::create_directories(fs::path(output_path).parent_path());
                    
                    std::cout << "Downloading: " << filename << "..." << std::endl;
                    
                    bool success = HttpClient::download_file(
                        file_url,
                        output_path,
                        [](size_t downloaded, size_t total) {
                            if (total > 0) {
                                int percent = (downloaded * 100) / total;
                                std::cout << "\rProgress: " << percent << "%" << std::flush;
                            }
                        },
                        headers
                    );
                    
                    if (success) {
                        std::cout << "\nDownloaded: " << filename << std::endl;
                    } else {
                        std::cerr << "\nFailed to download: " << filename << std::endl;
                    }
                }
            }
        } else {
            throw std::runtime_error("Failed to fetch model info from Hugging Face API");
        }
    } catch (const std::exception& e) {
        std::cerr << "Error downloading model: " << e.what() << std::endl;
        throw;
    }
}

void ModelManager::download_from_flm(const std::string& checkpoint, bool do_not_upgrade) {
    std::cout << "[ModelManager] Pulling FLM model: " << checkpoint << std::endl;
    
    // Find flm executable
    std::string flm_path;
#ifdef _WIN32
    // On Windows, check if flm.exe is in PATH
    flm_path = "flm";
#else
    // On Unix, check if flm is in PATH
    flm_path = "flm";
#endif
    
    // Prepare arguments
    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }
    
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // Run flm pull command
    auto handle = utils::ProcessManager::start_process(flm_path, args, "", false);
    
    // Wait for download to complete
    if (!utils::ProcessManager::is_running(handle)) {
        int exit_code = utils::ProcessManager::get_exit_code(handle);
        std::cerr << "[ModelManager ERROR] FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed");
    }
    
    // Wait for process to complete
    int timeout_seconds = 300; // 5 minutes
    std::cout << "[ModelManager] Waiting for FLM model download to complete..." << std::endl;
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        if (!utils::ProcessManager::is_running(handle)) {
            int exit_code = utils::ProcessManager::get_exit_code(handle);
            if (exit_code != 0) {
                std::cerr << "[ModelManager ERROR] FLM pull failed with exit code: " << exit_code << std::endl;
                throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            std::cout << "[ModelManager] Still downloading... (" << (i/10) << "s elapsed)" << std::endl;
        }
    }
    
    std::cout << "[ModelManager] FLM model pull completed successfully" << std::endl;
}

void ModelManager::delete_model(const std::string& model_name) {
    auto info = get_model_info(model_name);
    
    // Get Hugging Face cache directory
    std::string hf_cache;
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        hf_cache = std::string(hf_home_env) + "/hub";
    } else {
#ifdef _WIN32
        const char* userprofile = std::getenv("USERPROFILE");
        if (userprofile) {
            hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
        } else {
            throw std::runtime_error("Cannot determine HF cache directory");
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            hf_cache = std::string(home) + "/.cache/huggingface/hub";
        } else {
            throw std::runtime_error("Cannot determine HF cache directory");
        }
#endif
    }
    
    // Parse checkpoint to get repo ID
    std::string checkpoint = info.checkpoint;
    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        checkpoint = checkpoint.substr(0, colon_pos);
    }
    
    // Convert checkpoint to cache directory name
    std::string cache_dir_name = "models--";
    for (char c : checkpoint) {
        if (c == '/') {
            cache_dir_name += "--";
        } else {
            cache_dir_name += c;
        }
    }
    
    std::string model_cache_path = hf_cache + "/" + cache_dir_name;
    
    if (fs::exists(model_cache_path)) {
        fs::remove_all(model_cache_path);
        std::cout << "Deleted model: " << model_name << std::endl;
    }
    
    // Remove from user models if it's a user model
    if (model_name.substr(0, 5) == "user.") {
        std::string clean_name = model_name.substr(5);
        json updated_user_models = user_models_;
        updated_user_models.erase(clean_name);
        save_user_models(updated_user_models);
        user_models_ = updated_user_models;
    }
}

ModelInfo ModelManager::get_model_info(const std::string& model_name) {
    auto models = get_supported_models();
    
    if (models.find(model_name) != models.end()) {
        return models[model_name];
    }
    
    throw std::runtime_error("Model not found: " + model_name);
}

bool ModelManager::model_exists(const std::string& model_name) {
    auto models = get_supported_models();
    return models.find(model_name) != models.end();
}

} // namespace lemon

