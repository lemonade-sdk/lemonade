#include <lemon/model_manager.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/system_info.h>
#include <lemon/backends/fastflowlm_server.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <iomanip>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

// Helper functions for string operations
static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static bool ends_with_ignore_case(const std::string& str, const std::string& suffix) {
    if (suffix.length() > str.length()) {
        return false;
    }
    return to_lower(str.substr(str.length() - suffix.length())) == to_lower(suffix);
}

static bool starts_with_ignore_case(const std::string& str, const std::string& prefix) {
    if (prefix.length() > str.length()) {
        return false;
    }
    return to_lower(str.substr(0, prefix.length())) == to_lower(prefix);
}

static bool contains_ignore_case(const std::string& str, const std::string& substr) {
    return to_lower(str).find(to_lower(substr)) != std::string::npos;
}

// Structure to hold identified GGUF files

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
    
    // Default to ~/.cache/lemonade (matching Python implementation)
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
    return "/tmp/.cache/lemonade";
#endif
}

std::string ModelManager::get_user_models_file() {
    return get_cache_dir() + "/user_models.json";
}

std::string ModelManager::get_hf_cache_dir() const {
    // Check HF_HUB_CACHE first (highest priority)
    const char* hf_hub_cache_env = std::getenv("HF_HUB_CACHE");
    if (hf_hub_cache_env) {
        return std::string(hf_hub_cache_env);
    }
    
    // Check HF_HOME second (append /hub)
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        return std::string(hf_home_env) + "/hub";
    }
    
    // Default platform-specific paths
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.cache\\huggingface\\hub";
    }
    return "C:\\.cache\\huggingface\\hub";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/huggingface/hub";
    }
    return "/tmp/.cache/huggingface/hub";
#endif
}

std::string ModelManager::resolve_model_path(const ModelInfo& info) const {
    // FLM models use checkpoint as-is (e.g., "gemma3:4b")
    if (info.recipe == "flm") {
        return info.checkpoint;
    }

    // Local path models use checkpoint as-is (absolute path to file)
    if (info.source == "local_path") {
        return info.checkpoint;
    }

    std::string hf_cache = get_hf_cache_dir();

    // Local uploads: checkpoint is relative path from HF cache
    if (info.source == "local_upload") {
        std::string normalized = info.checkpoint;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return hf_cache + "/" + normalized;
    }
    
    // HuggingFace models: need to find the GGUF file in cache
    // Parse checkpoint to get repo_id and variant
    std::string repo_id = info.checkpoint;
    std::string variant;
    
    size_t colon_pos = info.checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = info.checkpoint.substr(0, colon_pos);
        variant = info.checkpoint.substr(colon_pos + 1);
    }
    
    // Convert org/model to models--org--model
    std::string cache_dir_name = "models--";
    for (char c : repo_id) {
        cache_dir_name += (c == '/') ? "--" : std::string(1, c);
    }
    
    std::string model_cache_path = hf_cache + "/" + cache_dir_name;
    
    // For OGA models, look for genai_config.json directory
    if (info.recipe.find("oga-") == 0 || info.recipe == "ryzenai") {
        if (fs::exists(model_cache_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(model_cache_path)) {
                if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                    return entry.path().parent_path().string();
                }
            }
        }
        return model_cache_path;  // Return directory even if genai_config not found
    }

    // For whispercpp, find the .bin model file
    if (info.recipe == "whispercpp") {
        if (!fs::exists(model_cache_path)) {
            return model_cache_path;  // Return directory path even if not found
        }

        // Collect all .bin files
        std::vector<std::string> all_bin_files;
        for (const auto& entry : fs::recursive_directory_iterator(model_cache_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(".bin") != std::string::npos) {
                    all_bin_files.push_back(entry.path().string());
                }
            }
        }

        if (all_bin_files.empty()) {
            return model_cache_path;  // Return directory if no .bin found
        }

        // Sort files for consistent ordering
        std::sort(all_bin_files.begin(), all_bin_files.end());

        // If variant specified, try to match it
        if (!variant.empty()) {
            for (const auto& filepath : all_bin_files) {
                std::string filename = fs::path(filepath).filename().string();
                if (filename == variant) {
                    return filepath;
                }
            }
        }

        // Return first .bin file as fallback
        return all_bin_files[0];
    }

    // For llamacpp, find the GGUF file with advanced sharded model support
    if (info.recipe == "llamacpp") {
        if (!fs::exists(model_cache_path)) {
            return model_cache_path;  // Return directory path even if not found
        }
        
        // Collect all GGUF files (exclude mmproj files)
        std::vector<std::string> all_gguf_files;
        for (const auto& entry : fs::recursive_directory_iterator(model_cache_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string filename_lower = filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
                
                if (filename.find(".gguf") != std::string::npos && filename_lower.find("mmproj") == std::string::npos) {
                    all_gguf_files.push_back(entry.path().string());
                }
            }
        }
        
        if (all_gguf_files.empty()) {
            return model_cache_path;  // Return directory if no GGUF found
        }
        
        // Sort files for consistent ordering (important for sharded models)
        std::sort(all_gguf_files.begin(), all_gguf_files.end());
        
        // Case 0: Wildcard (*) - return first file (llama-server will auto-load shards)
        if (variant == "*") {
            return all_gguf_files[0];
        }
        
        // Case 1: Empty variant - return first file
        if (variant.empty()) {
            return all_gguf_files[0];
        }
        
        // Case 2: Exact filename match (variant ends with .gguf)
        if (variant.find(".gguf") != std::string::npos) {
            for (const auto& filepath : all_gguf_files) {
                std::string filename = fs::path(filepath).filename().string();
                if (filename == variant) {
                    return filepath;
                }
            }
            return model_cache_path;  // Not found
        }
        
        // Case 3: Files ending with {variant}.gguf (case insensitive)
        std::string variant_lower = variant;
        std::transform(variant_lower.begin(), variant_lower.end(), variant_lower.begin(), ::tolower);
        std::string suffix = variant_lower + ".gguf";
        
        std::vector<std::string> matching_files;
        for (const auto& filepath : all_gguf_files) {
            std::string filename = fs::path(filepath).filename().string();
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
            
            if (filename_lower.size() >= suffix.size() &&
                filename_lower.substr(filename_lower.size() - suffix.size()) == suffix) {
                matching_files.push_back(filepath);
            }
        }
        
        if (!matching_files.empty()) {
            return matching_files[0];
        }
        
        // Case 4: Folder-based sharding (files in variant/ folder)
        std::string folder_prefix_lower = variant_lower + "/";
        
        for (const auto& filepath : all_gguf_files) {
            // Get relative path from model cache path
            std::string relative_path = filepath.substr(model_cache_path.length());
            std::string relative_lower = relative_path;
            std::transform(relative_lower.begin(), relative_lower.end(), relative_lower.begin(), ::tolower);
            
            if (relative_lower.find(folder_prefix_lower) != std::string::npos) {
                return filepath;
            }
        }
        
        // No match found - return first file as fallback
        return all_gguf_files[0];
    }
    
    // Fallback: return directory path
    return model_cache_path;
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
    // Build cache if needed (lazy initialization)
    build_cache();
    
    // Return copy of cache (all models, including their download status)
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    return models_cache_;
}

void ModelManager::build_cache() {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    
    if (cache_valid_) {
        return;
    }
    
    std::cout << "[ModelManager] Building models cache..." << std::endl;
    
    models_cache_.clear();
    std::map<std::string, ModelInfo> all_models;
    
    // Step 1: Load ALL models from JSON (server models)
    for (auto& [key, value] : server_models_.items()) {
        ModelInfo info;
        info.model_name = key;
        info.checkpoint = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = JsonUtils::get_or_default<bool>(value, "suggested", false);
        info.mmproj = JsonUtils::get_or_default<std::string>(value, "mmproj", "");
        info.size = JsonUtils::get_or_default<double>(value, "size", 0.0);
        
        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }
        
        // Populate type and device fields (multi-model support)
        info.type = get_model_type_from_labels(info.labels);
        info.device = get_device_type_from_recipe(info.recipe);
        
        info.resolved_path = resolve_model_path(info);
        all_models[key] = info;
    }
    
    // Load user models with "user." prefix
    for (auto& [key, value] : user_models_.items()) {
        ModelInfo info;
        info.model_name = "user." + key;
        info.checkpoint = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = JsonUtils::get_or_default<bool>(value, "suggested", true);
        info.mmproj = JsonUtils::get_or_default<std::string>(value, "mmproj", "");
        info.source = JsonUtils::get_or_default<std::string>(value, "source", "");
        info.size = JsonUtils::get_or_default<double>(value, "size", 0.0);
        
        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }
        
        // Populate type and device fields (multi-model support)
        info.type = get_model_type_from_labels(info.labels);
        info.device = get_device_type_from_recipe(info.recipe);
        
        info.resolved_path = resolve_model_path(info);
        all_models[info.model_name] = info;
    }
    
    // Step 2: Filter by backend availability
    all_models = filter_models_by_backend(all_models);
    
    // Step 3: Check download status ONCE for all models
    auto flm_models = get_flm_installed_models();
    std::unordered_set<std::string> flm_set(flm_models.begin(), flm_models.end());
    
    int downloaded_count = 0;
    for (auto& [name, info] : all_models) {
        if (info.recipe == "flm") {
            info.downloaded = flm_set.count(info.checkpoint) > 0;
        } else {
            // Check if model file/dir exists
            bool file_exists = !info.resolved_path.empty() && fs::exists(info.resolved_path);
            
            if (file_exists) {
                // Also check for incomplete downloads:
                // 1. Check for .download_manifest.json in snapshot directory
                // 2. Check for any .partial files
                fs::path resolved(info.resolved_path);
                
                // For directories (OGA models), check within the directory
                // For files (GGUF models), check in parent directory
                fs::path snapshot_dir = fs::is_directory(resolved) ? resolved : resolved.parent_path();
                
                // Check for manifest (indicates incomplete multi-file download)
                fs::path manifest_path = snapshot_dir / ".download_manifest.json";
                bool has_manifest = fs::exists(manifest_path);
                
                // Check for .partial files
                bool has_partial = false;
                if (fs::is_directory(resolved)) {
                    // For directories, scan for any .partial files inside
                    for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
                        if (entry.path().extension() == ".partial") {
                            has_partial = true;
                            break;
                        }
                    }
                } else {
                    // For files, check if the specific file has a .partial version
                    has_partial = fs::exists(info.resolved_path + ".partial");
                }
                
                info.downloaded = !has_manifest && !has_partial;
            } else {
                info.downloaded = false;
            }
        }
        
        if (info.downloaded) {
            downloaded_count++;
        }
        
        models_cache_[name] = info;
    }
    
    cache_valid_ = true;
    std::cout << "[ModelManager] Cache built: " << models_cache_.size() 
              << " total, " << downloaded_count << " downloaded" << std::endl;
}

void ModelManager::add_model_to_cache(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    
    if (!cache_valid_) {
        return; // Will initialize on next access
    }
    
    // Parse model name to get JSON key
    std::string json_key = model_name;
    bool is_user_model = model_name.substr(0, 5) == "user.";
    if (is_user_model) {
        json_key = model_name.substr(5);
    }
    
    // Find in JSON
    json* model_json = nullptr;
    if (is_user_model && user_models_.contains(json_key)) {
        model_json = &user_models_[json_key];
    } else if (!is_user_model && server_models_.contains(json_key)) {
        model_json = &server_models_[json_key];
    }
    
    if (!model_json) {
        std::cerr << "[ModelManager] Warning: '" << model_name << "' not found in JSON" << std::endl;
        return;
    }
    
    // Build ModelInfo
    ModelInfo info;
    info.model_name = model_name;
    info.checkpoint = JsonUtils::get_or_default<std::string>(*model_json, "checkpoint", "");
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", is_user_model);
    info.mmproj = JsonUtils::get_or_default<std::string>(*model_json, "mmproj", "");
    info.source = JsonUtils::get_or_default<std::string>(*model_json, "source", "");
    
    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            info.labels.push_back(label.get<std::string>());
        }
    }
    
    // Populate type and device fields (multi-model support)
    info.type = get_model_type_from_labels(info.labels);
    info.device = get_device_type_from_recipe(info.recipe);
    
    info.resolved_path = resolve_model_path(info);
    
    // Check if it should be filtered out by backend availability
    std::map<std::string, ModelInfo> temp_map = {{model_name, info}};
    auto filtered = filter_models_by_backend(temp_map);
    
    if (filtered.empty()) {
        std::cout << "[ModelManager] Model '" << model_name << "' filtered out by backend availability" << std::endl;
        return; // Backend not available, don't add to cache
    }
    
    // Check download status
    if (info.recipe == "flm") {
        auto flm_models = get_flm_installed_models();
        info.downloaded = std::find(flm_models.begin(), flm_models.end(), info.checkpoint) != flm_models.end();
    } else {
        bool file_exists = !info.resolved_path.empty() && fs::exists(info.resolved_path);
        
        if (file_exists) {
            // Check for incomplete downloads
            fs::path resolved(info.resolved_path);
            fs::path snapshot_dir = fs::is_directory(resolved) ? resolved : resolved.parent_path();
            
            fs::path manifest_path = snapshot_dir / ".download_manifest.json";
            bool has_manifest = fs::exists(manifest_path);
            
            bool has_partial = false;
            if (fs::is_directory(resolved)) {
                for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
                    if (entry.path().extension() == ".partial") {
                        has_partial = true;
                        break;
                    }
                }
            } else {
                has_partial = fs::exists(info.resolved_path + ".partial");
            }
            
            info.downloaded = !has_manifest && !has_partial;
        } else {
            info.downloaded = false;
        }
    }
    
    models_cache_[model_name] = info;
    std::cout << "[ModelManager] Added '" << model_name << "' to cache (downloaded=" << info.downloaded << ")" << std::endl;
}

void ModelManager::update_model_in_cache(const std::string& model_name, bool downloaded) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    
    if (!cache_valid_) {
        return; // Will rebuild on next access
    }
    
    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        it->second.downloaded = downloaded;
        
        // Recompute resolved_path after download
        // The path changes now that files exist on disk
        if (downloaded) {
            it->second.resolved_path = resolve_model_path(it->second);
            std::cout << "[ModelManager] Updated '" << model_name 
                      << "' downloaded=" << downloaded 
                      << ", resolved_path=" << it->second.resolved_path << std::endl;
        } else {
            std::cout << "[ModelManager] Updated '" << model_name 
                      << "' downloaded=" << downloaded << std::endl;
        }
    } else {
        std::cerr << "[ModelManager] Warning: '" << model_name << "' not found in cache" << std::endl;
    }
}

void ModelManager::remove_model_from_cache(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    
    if (!cache_valid_) {
        return;
    }
    
    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        if (it->second.source == "local_upload") {
            // Local upload - remove entirely from cache
            models_cache_.erase(model_name);
            std::cout << "[ModelManager] Removed '" << model_name << "' from cache" << std::endl;
        } else {
            // Registered model - just mark as not downloaded
            it->second.downloaded = false;
            std::cout << "[ModelManager] Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}

std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    // Build cache if needed
    build_cache();
    
    // Filter and return only downloaded models
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    std::map<std::string, ModelInfo> downloaded;
    for (const auto& [name, info] : models_cache_) {
        if (info.downloaded) {
            downloaded[name] = info;
        }
    }
    return downloaded;
}

// Helper function to check if NPU is available
// Matches Python behavior: on Windows, assume available (FLM will fail at runtime if not compatible)
// This allows showing FLM models on Windows systems - the actual compatibility check happens when loading
static bool is_npu_available(const json& hardware) {
    // Check if user explicitly disabled NPU check
    const char* skip_check = std::getenv("RYZENAI_SKIP_PROCESSOR_CHECK");
    if (skip_check && (std::string(skip_check) == "1" || 
                       std::string(skip_check) == "true" || 
                       std::string(skip_check) == "yes")) {
        return true;
    }
    
    // Use provided hardware info
    if (hardware.contains("npu") && hardware["npu"].is_object()) {
        return hardware["npu"].value("available", false);
    }
    
    return false;
}

static bool is_flm_available(const json& hardware) {
    // FLM models are available if NPU hardware is present
    // The FLM executable will be obtained as needed
    return is_npu_available(hardware);
}

static bool is_oga_available(const json& hardware) {
    // OGA models are available if NPU hardware is present
    // The ryzenai-server executable (OGA backend) will be obtained as needed
    return is_npu_available(hardware);
}

// Helper function to parse physical memory string (e.g., "32.00 GB") to GB as double
// Returns 0.0 if parsing fails
static double parse_physical_memory_gb(const std::string& memory_str) {
    if (memory_str.empty()) {
        return 0.0;
    }
    
    // Expected format: "XX.XX GB" or "XX GB"
    std::istringstream iss(memory_str);
    double value = 0.0;
    std::string unit;
    
    if (iss >> value >> unit) {
        // Convert to lowercase for comparison
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        if (unit == "gb") {
            return value;
        } else if (unit == "mb") {
            return value / 1024.0;
        } else if (unit == "tb") {
            return value * 1024.0;
        }
    }
    
    return 0.0;
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
    
    // Get hardware info once (this will print the message)
    json system_info = SystemInfoCache::get_system_info_with_cache(false);
    json hardware = system_info.contains("devices") ? system_info["devices"] : json::object();
    
    // Check backend availability (passing hardware info)
    bool npu_available = is_npu_available(hardware);
    bool flm_available = is_flm_available(hardware);
    bool oga_available = is_oga_available(hardware);
    
    // Get system RAM for memory-based filtering
    double system_ram_gb = 0.0;
    if (system_info.contains("Physical Memory") && system_info["Physical Memory"].is_string()) {
        system_ram_gb = parse_physical_memory_gb(system_info["Physical Memory"].get<std::string>());
    }
    double max_model_size_gb = system_ram_gb * 0.8;  // 80% of system RAM
    
    // Debug output (only shown once during startup)
    static bool debug_printed = false;
    if (!debug_printed) {
        std::cout << "[ModelManager] Backend availability:" << std::endl;
        std::cout << "  - NPU hardware: " << (npu_available ? "Yes" : "No") << std::endl;
        std::cout << "  - FLM available: " << (flm_available ? "Yes" : "No") << std::endl;
        std::cout << "  - OGA available: " << (oga_available ? "Yes" : "No") << std::endl;
        if (system_ram_gb > 0.0) {
            std::cout << "  - System RAM: " << std::fixed << std::setprecision(1) << system_ram_gb 
                      << " GB (max model size: " << max_model_size_gb << " GB)" << std::endl;
        }
        debug_printed = true;
    }
    
    int filtered_count = 0;
    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        bool filter_out = false;
        std::string filter_reason;
        
        // Filter FLM models based on NPU availability
        if (recipe == "flm") {
            if (!flm_available) {
                filter_out = true;
                filter_reason = "FLM not available";
            }
        }
        
        // Filter OGA models based on NPU availability
        if (recipe == "oga-npu" || recipe == "oga-hybrid" || recipe == "oga-cpu") {
            if (!oga_available) {
                filter_out = true;
                filter_reason = "OGA not available";
            }
        }
        
        // Filter out other OGA models (not yet implemented)
        if (recipe == "oga-igpu") {
            filter_out = true;
            filter_reason = "oga-igpu not implemented";
        }
        
        // On macOS, only show llamacpp models
        if (is_macos && recipe != "llamacpp") {
            filter_out = true;
            filter_reason = "macOS only supports llamacpp";
        }
        
        // Filter out models that are too large for system RAM
        // Heuristic: if model size > 80% of system RAM, filter it out
        if (!filter_out && system_ram_gb > 0.0 && info.size > 0.0) {
            if (info.size > max_model_size_gb) {
                filter_out = true;
                filter_reason = "Model too large for system RAM";
            }
        }
        
        // Special rule: filter out gpt-oss-20b-FLM on systems with less than 64 GB RAM
        if (!filter_out && name == "gpt-oss-20b-FLM" && system_ram_gb > 0.0 && system_ram_gb < 64.0) {
            filter_out = true;
            filter_reason = "gpt-oss-20b-FLM requires 64 GB RAM";
        }
        
        if (filter_out) {
            filtered_count++;
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
                                      bool embedding,
                                      bool reranking,
                                      const std::string& mmproj,
                                      const std::string& source) {
    
    // Remove "user." prefix if present
    std::string clean_name = model_name;
    if (clean_name.substr(0, 5) == "user.") {
        clean_name = clean_name.substr(5);
    }
    
    json model_entry;
    model_entry["checkpoint"] = checkpoint;
    model_entry["recipe"] = recipe;
    model_entry["suggested"] = true;  // Always set suggested=true for user models
    
    // Always start with "custom" label (matching Python implementation)
    std::vector<std::string> labels = {"custom"};
    if (reasoning) {
        labels.push_back("reasoning");
    }
    if (vision) {
        labels.push_back("vision");
    }
    if (embedding) {
        labels.push_back("embeddings");
    }
    if (reranking) {
        labels.push_back("reranking");
    }
    model_entry["labels"] = labels;
    
    if (!mmproj.empty()) {
        model_entry["mmproj"] = mmproj;
    }
    
    if (!source.empty()) {
        model_entry["source"] = source;
    }
    
    json updated_user_models = user_models_;
    updated_user_models[clean_name] = model_entry;
    
    save_user_models(updated_user_models);
    user_models_ = updated_user_models;
    
    // Add new model to cache incrementally
    add_model_to_cache("user." + clean_name);
}

// Helper function to get FLM installed models by calling 'flm list --filter installed --quiet'
// Uses the improved FLM CLI methodology with --filter and --quiet flags
std::vector<std::string> ModelManager::get_flm_installed_models() {
    std::vector<std::string> installed_models;

    // Find the flm executable using shared utility
    std::string flm_path = utils::find_flm_executable();
    if (flm_path.empty()) {
        return installed_models; // FLM not installed
    }

    // Run 'flm list --filter installed --quiet' to get only installed models
    // Use the full path to flm.exe to avoid PATH issues
    std::string command = "\"" + flm_path + "\" list --filter installed --quiet";
    
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
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

    // Parse output - cleaner format without emojis
    // Expected format:
    //   Models:
    //     - modelname:tag
    //     - another:model
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Skip the "Models:" header line or empty lines
        if (line == "Models:" || line.empty()) {
            continue;
        }

        // Parse model checkpoint (format: "  - modelname:tag")
        if (line.find("- ") == 0) {
            std::string checkpoint = line.substr(2);
            // Trim any remaining whitespace
            checkpoint.erase(0, checkpoint.find_first_not_of(" \t"));
            checkpoint.erase(checkpoint.find_last_not_of(" \t") + 1);
            if (!checkpoint.empty()) {
                installed_models.push_back(checkpoint);
            }
        }
    }

    return installed_models;
}

bool ModelManager::is_model_downloaded(const std::string& model_name) {
    // Build cache if needed
    build_cache();
    
    // O(1) lookup - download status is in cache
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        return it->second.downloaded;
    }
    return false;
}

bool ModelManager::is_model_downloaded(const std::string& model_name, 
                                       const std::vector<std::string>* flm_cache) {
    // This overload is no longer needed with unified cache, but keep for compatibility
    // Just delegate to the simpler version
    return is_model_downloaded(model_name);
}

void ModelManager::delete_model(const std::string& model_name) {
    auto info = get_model_info(model_name);
    
    std::cout << "[ModelManager] Deleting model: " << model_name << std::endl;
    std::cout << "[ModelManager] Checkpoint: " << info.checkpoint << std::endl;
    std::cout << "[ModelManager] Recipe: " << info.recipe << std::endl;
    
    // Handle FLM models separately
    if (info.recipe == "flm") {
        std::cout << "[ModelManager] Deleting FLM model: " << info.checkpoint << std::endl;
        
        // Validate checkpoint is not empty
        if (info.checkpoint.empty()) {
            throw std::runtime_error("FLM model has empty checkpoint field, cannot delete");
        }
        
        // Find flm executable
        std::string flm_path;
#ifdef _WIN32
        flm_path = "flm";
#else
        flm_path = "flm";
#endif
        
        // Prepare arguments for 'flm remove' command
        std::vector<std::string> args = {"remove", info.checkpoint};
        
        std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
        for (const auto& arg : args) {
            std::cout << " \"" << arg << "\"";
        }
        std::cout << std::endl;
        
        // Run flm remove command
        auto handle = utils::ProcessManager::start_process(flm_path, args, "", false);
        
        // Wait for process to complete
        int timeout_seconds = 60; // 1 minute timeout for removal
        for (int i = 0; i < timeout_seconds * 10; ++i) {
            if (!utils::ProcessManager::is_running(handle)) {
                int exit_code = utils::ProcessManager::get_exit_code(handle);
                if (exit_code != 0) {
                    std::cerr << "[ModelManager ERROR] FLM remove failed with exit code: " << exit_code << std::endl;
                    throw std::runtime_error("Failed to delete FLM model " + model_name + ": FLM remove failed with exit code " + std::to_string(exit_code));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Check if process is still running (timeout)
        if (utils::ProcessManager::is_running(handle)) {
            std::cerr << "[ModelManager ERROR] FLM remove timed out" << std::endl;
            throw std::runtime_error("Failed to delete FLM model " + model_name + ": FLM remove timed out");
        }
        
        std::cout << "[ModelManager] ✓ Successfully deleted FLM model: " << model_name << std::endl;
        
        // Remove from user models if it's a user model
        if (model_name.substr(0, 5) == "user.") {
            std::string clean_name = model_name.substr(5);
            json updated_user_models = user_models_;
            updated_user_models.erase(clean_name);
            save_user_models(updated_user_models);
            user_models_ = updated_user_models;
            std::cout << "[ModelManager] ✓ Removed from user_models.json" << std::endl;
        }
        
        // Remove from cache after successful deletion
        remove_model_from_cache(model_name);
        
        return;
    }
    
    // Use resolved_path to find the model directory to delete
    if (info.resolved_path.empty()) {
        throw std::runtime_error("Model has no resolved_path, cannot determine files to delete");
    }
    
    // Find the models--* directory from resolved_path
    // resolved_path could be a file or directory, we need to find the models-- ancestor
    fs::path path_obj(info.resolved_path);
    std::string model_cache_path;
    
    // Walk up the directory tree to find models--* directory
    while (!path_obj.empty() && path_obj.has_filename()) {
        std::string dirname = path_obj.filename().string();
        if (dirname.find("models--") == 0) {
            model_cache_path = path_obj.string();
            break;
        }
        path_obj = path_obj.parent_path();
    }
    
    if (model_cache_path.empty()) {
        throw std::runtime_error("Could not find models-- directory in path: " + info.resolved_path);
    }
    
    std::cout << "[ModelManager] Cache path: " << model_cache_path << std::endl;
    
    if (fs::exists(model_cache_path)) {
        std::cout << "[ModelManager] Removing directory..." << std::endl;
        fs::remove_all(model_cache_path);
        std::cout << "[ModelManager] ✓ Deleted model files: " << model_name << std::endl;
    } else {
        std::cout << "[ModelManager] Warning: Model cache directory not found (may already be deleted)" << std::endl;
    }
    
    // Remove from user models if it's a user model
    if (model_name.substr(0, 5) == "user.") {
        std::string clean_name = model_name.substr(5);
        json updated_user_models = user_models_;
        updated_user_models.erase(clean_name);
        save_user_models(updated_user_models);
        user_models_ = updated_user_models;
        std::cout << "[ModelManager] ✓ Removed from user_models.json" << std::endl;
    }
    
    // Remove from cache after successful deletion
    remove_model_from_cache(model_name);
}

ModelInfo ModelManager::get_model_info(const std::string& model_name) {
    // Build cache if needed
    build_cache();
    
    // O(1) lookup in cache
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        return it->second;
    }
    
    throw std::runtime_error("Model not found: " + model_name);
}

bool ModelManager::model_exists(const std::string& model_name) {
    // Build cache if needed
    build_cache();
    
    // O(1) lookup in cache
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    return models_cache_.find(model_name) != models_cache_.end();
}

} // namespace lemon

