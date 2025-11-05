#include <lemon/model_manager.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/system_info.h>
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
struct GGUFFiles {
    std::map<std::string, std::string> core_files;  // {"variant": "file.gguf", "mmproj": "file.mmproj"}
    std::vector<std::string> sharded_files;         // Additional shard files
};

// Identifies GGUF model files matching the variant (Python equivalent of identify_gguf_models)
static GGUFFiles identify_gguf_models(
    const std::string& checkpoint,
    const std::string& variant,
    const std::string& mmproj,
    const std::vector<std::string>& repo_files
) {
    const std::string hint = R"(
    The CHECKPOINT:VARIANT scheme is used to specify model files in Hugging Face repositories.

    The VARIANT format can be one of several types:
    0. wildcard (*): download all .gguf files in the repo
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)

    Examples:
    - "ggml-org/gpt-oss-120b-GGUF:*" -> downloads all .gguf files in repo
    - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
    - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
    )";

    GGUFFiles result;
    std::vector<std::string> sharded_files;
    std::string variant_name;

    // (case 0) Wildcard, download everything
    if (!variant.empty() && variant == "*") {
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, ".gguf")) {
                sharded_files.push_back(f);
            }
        }
        
        if (sharded_files.empty()) {
            throw std::runtime_error("No .gguf files found in repository " + checkpoint + ". " + hint);
        }
        
        // Sort to ensure consistent ordering
        std::sort(sharded_files.begin(), sharded_files.end());
        
        // Use first file as primary (this is how llamacpp handles it)
        variant_name = sharded_files[0];
    }
    // (case 1) If variant ends in .gguf, use it directly
    else if (!variant.empty() && ends_with_ignore_case(variant, ".gguf")) {
        variant_name = variant;
        
        // Validate file exists in repo
        bool found = false;
        for (const auto& f : repo_files) {
            if (f == variant) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            throw std::runtime_error(
                "File " + variant + " not found in Hugging Face repository " + checkpoint + ". " + hint
            );
        }
    }
    // (case 2) If no variant is provided, get the first .gguf file in the repository
    else if (variant.empty()) {
        std::vector<std::string> all_variants;
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, ".gguf") && !contains_ignore_case(f, "mmproj")) {
                all_variants.push_back(f);
            }
        }
        
        if (all_variants.empty()) {
            throw std::runtime_error(
                "No .gguf files found in Hugging Face repository " + checkpoint + ". " + hint
            );
        }
        
        variant_name = all_variants[0];
    }
    else {
        // (case 3) Find a single file ending with the variant name (case insensitive)
        std::vector<std::string> end_with_variant;
        std::string variant_suffix = variant + ".gguf";
        
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, variant_suffix) && !contains_ignore_case(f, "mmproj")) {
                end_with_variant.push_back(f);
            }
        }
        
        if (end_with_variant.size() == 1) {
            variant_name = end_with_variant[0];
        }
        else if (end_with_variant.size() > 1) {
            throw std::runtime_error(
                "Multiple .gguf files found for variant " + variant + ", but only one is allowed. " + hint
            );
        }
        // (case 4) Check whether the variant corresponds to a folder with sharded files (case insensitive)
        else {
            std::string folder_prefix = variant + "/";
            for (const auto& f : repo_files) {
                if (ends_with_ignore_case(f, ".gguf") && starts_with_ignore_case(f, folder_prefix)) {
                    sharded_files.push_back(f);
                }
            }
            
            if (sharded_files.empty()) {
                throw std::runtime_error(
                    "No .gguf files found for variant " + variant + ". " + hint
                );
            }
            
            // Sort to ensure consistent ordering
            std::sort(sharded_files.begin(), sharded_files.end());
            
            // Use first file as primary (this is how llamacpp handles it)
            variant_name = sharded_files[0];
        }
    }
    
    result.core_files["variant"] = variant_name;
    result.sharded_files = sharded_files;
    
    // If there is a mmproj file, add it to the core files
    if (!mmproj.empty()) {
        bool found = false;
        for (const auto& f : repo_files) {
            if (f == mmproj) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            throw std::runtime_error(
                "The provided mmproj file " + mmproj + " was not found in " + checkpoint + "."
            );
        }
        
        result.core_files["mmproj"] = mmproj;
    }
    
    return result;
}

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
    const char* hf_home_env = std::getenv("HF_HOME");
    if (hf_home_env) {
        return std::string(hf_home_env) + "/hub";
    }
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
        
        // Resolve model path
        info.resolved_path = resolve_model_path(info);
        
        models[key] = info;
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
        
        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }
        
        // Resolve model path
        info.resolved_path = resolve_model_path(info);
        
        models[info.model_name] = info;
    }
    
    // Filter by backend availability before returning
    return filter_models_by_backend(models);
}

std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    auto all_models = get_supported_models(); // Already filtered by backend
    std::map<std::string, ModelInfo> downloaded;
    
    // OPTIMIZATION: For FLM, get the list once
    auto flm_models = get_flm_installed_models();
    std::unordered_set<std::string> available_flm_models(flm_models.begin(), flm_models.end());
    
    // Filter models - just check resolved_path exists
    for (const auto& [name, info] : all_models) {
        bool is_available = false;
        
        if (info.recipe == "flm") {
            // FLM models use their own registry
            is_available = available_flm_models.count(info.checkpoint) > 0;
        } else {
            // All other models: check if resolved_path exists
            is_available = !info.resolved_path.empty() && fs::exists(info.resolved_path);
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
// Helper function to get backend availability from SystemInfo
static json get_backend_availability() {
    // Try to load from cache first
    SystemInfoCache cache;
    json cached_hardware = cache.load_hardware_info();
    
    if (!cached_hardware.empty()) {
        return cached_hardware;
    }
    
    // If no cache, detect hardware
    auto sys_info = create_system_info();
    json hardware = sys_info->get_device_dict();
    
    // Strip inference_engines before caching (hardware only)
    json hardware_only = hardware;
    if (hardware_only.contains("cpu") && hardware_only["cpu"].contains("inference_engines")) {
        hardware_only["cpu"].erase("inference_engines");
    }
    if (hardware_only.contains("amd_igpu") && hardware_only["amd_igpu"].contains("inference_engines")) {
        hardware_only["amd_igpu"].erase("inference_engines");
    }
    if (hardware_only.contains("amd_dgpu") && hardware_only["amd_dgpu"].is_array()) {
        for (auto& gpu : hardware_only["amd_dgpu"]) {
            if (gpu.contains("inference_engines")) {
                gpu.erase("inference_engines");
            }
        }
    }
    if (hardware_only.contains("nvidia_dgpu") && hardware_only["nvidia_dgpu"].is_array()) {
        for (auto& gpu : hardware_only["nvidia_dgpu"]) {
            if (gpu.contains("inference_engines")) {
                gpu.erase("inference_engines");
            }
        }
    }
    if (hardware_only.contains("npu") && hardware_only["npu"].contains("inference_engines")) {
        hardware_only["npu"].erase("inference_engines");
    }
    
    cache.save_hardware_info(hardware_only);
    
    return hardware_only;
}

static bool is_npu_available() {
    // Check if user explicitly disabled NPU check
    const char* skip_check = std::getenv("RYZENAI_SKIP_PROCESSOR_CHECK");
    if (skip_check && (std::string(skip_check) == "1" || 
                       std::string(skip_check) == "true" || 
                       std::string(skip_check) == "yes")) {
        return true;
    }
    
    // Use SystemInfo to detect NPU
    json hardware = get_backend_availability();
    if (hardware.contains("npu") && hardware["npu"].is_object()) {
        return hardware["npu"].value("available", false);
    }
    
    return false;
}

static bool is_flm_available() {
    // FLM models are available if NPU hardware is present
    // The FLM executable will be obtained as needed
    return is_npu_available();
}

static bool is_oga_available() {
    // OGA models are available if NPU hardware is present
    // The ryzenai-server executable (OGA backend) will be obtained as needed
    return is_npu_available();
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
    bool npu_available = is_npu_available();
    bool flm_available = is_flm_available();
    bool oga_available = is_oga_available();
    
    // Debug output (only shown once during startup)
    static bool debug_printed = false;
    if (!debug_printed) {
        std::cout << "[ModelManager] Backend availability:" << std::endl;
        std::cout << "  - NPU hardware: " << (npu_available ? "Yes" : "No") << std::endl;
        std::cout << "  - FLM available: " << (flm_available ? "Yes" : "No") << std::endl;
        std::cout << "  - OGA available: " << (oga_available ? "Yes" : "No") << std::endl;
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
    
    // Check FLM models separately (they use FLM's own registry)
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
    
    // For all other models, just check if resolved_path exists
    return !info.resolved_path.empty() && fs::exists(info.resolved_path);
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
    
    // Check if model exists in registry
    bool model_registered = model_exists(model_name);
    
    if (!model_registered) {
        // Model not in registry - this must be a user model registration
        // Validate it has the "user." prefix
        if (model_name.substr(0, 5) != "user.") {
            throw std::runtime_error(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " + 
                model_name
            );
        }
        
        // Check that required arguments are provided
        if (actual_checkpoint.empty() || actual_recipe.empty()) {
            throw std::runtime_error(
                "Model " + model_name + " is not registered with Lemonade Server. "
                "To register and install it, provide the `checkpoint` and `recipe` "
                "arguments, as well as the optional `reasoning` and `mmproj` arguments "
                "as appropriate."
            );
        }
        
        // Validate GGUF models (llamacpp recipe) require a variant
        if (actual_recipe == "llamacpp") {
            std::string checkpoint_lower = actual_checkpoint;
            std::transform(checkpoint_lower.begin(), checkpoint_lower.end(), 
                          checkpoint_lower.begin(), ::tolower);
            if (checkpoint_lower.find("gguf") != std::string::npos && 
                actual_checkpoint.find(':') == std::string::npos) {
                throw std::runtime_error(
                    "You are required to provide a 'variant' in the checkpoint field when "
                    "registering a GGUF model. The variant is provided as CHECKPOINT:VARIANT. "
                    "For example: Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                    "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf"
                );
            }
        }
        
        std::cout << "Registering new user model: " << model_name << std::endl;
    } else {
        // Model is registered - if checkpoint not provided, look up from registry
        if (actual_checkpoint.empty()) {
            auto info = get_model_info(model_name);
            actual_checkpoint = info.checkpoint;
            actual_recipe = info.recipe;
        }
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
    
    // CRITICAL: If do_not_upgrade=true AND model is already downloaded, skip entirely
    // This prevents unnecessary HuggingFace API queries when we just want to use cached models
    // The do_not_upgrade flag means:
    //   - Load/inference endpoints: Don't check HuggingFace for updates (use cache if available)
    //   - Pull endpoint: Always check HuggingFace for latest version (do_not_upgrade=false)
    if (do_not_upgrade && is_model_downloaded(model_name)) {
        std::cout << "[ModelManager] Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }
    
    // Use FLM pull for FLM models, otherwise download from HuggingFace
    if (actual_recipe == "flm") {
        download_from_flm(actual_checkpoint, do_not_upgrade);
    } else if (actual_recipe == "llamacpp") {
        // For llamacpp (GGUF) models, use variant-aware download
        download_from_huggingface(repo_id, variant, mmproj);
    } else {
        // For non-GGUF models (oga-*, etc.), download all files (no variant filtering)
        download_from_huggingface(repo_id, "", "");
    }
    
    // Register if needed
    if (model_name.substr(0, 5) == "user." || !checkpoint.empty()) {
        register_user_model(model_name, actual_checkpoint, actual_recipe, 
                          reasoning, vision, mmproj);
    }
}

// Download model files from HuggingFace
// =====================================
// IMPORTANT: This function ALWAYS queries the HuggingFace API to get the repository
// file list, then downloads any missing files. It does NOT check do_not_upgrade.
//
// The caller (download_model) is responsible for checking do_not_upgrade and
// calling is_model_downloaded() before invoking this function.
//
// Download capabilities by backend:
//   - Lemonade Router (ModelManager): ✅ Downloads non-FLM models from HuggingFace
//   - FLM backend: ✅ Downloads FLM models via 'flm pull' command
//   - llama-server backend: ❌ Cannot download (expects GGUF files pre-cached)
//   - ryzenai-server backend: ❌ Cannot download (expects ONNX files pre-cached)
void ModelManager::download_from_huggingface(const std::string& repo_id,
                                            const std::string& variant,
                                            const std::string& mmproj) {
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
    std::string snapshot_path = model_cache_path + "/snapshots/main";
    fs::create_directories(snapshot_path);
    
    // Get HF token if available
    std::map<std::string, std::string> headers;
    const char* hf_token = std::getenv("HF_TOKEN");
    if (hf_token) {
        headers["Authorization"] = "Bearer " + std::string(hf_token);
    }
    
    // Query HuggingFace API to get list of all files in the repository
    // NOTE: This API call happens EVERY time this function is called, regardless of
    // whether files are cached. The do_not_upgrade check should happen in the caller
    // (download_model) to avoid this API call when using cached models.
    std::string api_url = "https://huggingface.co/api/models/" + repo_id;
    
    try {
        std::cout << "[ModelManager] Fetching repository file list from Hugging Face..." << std::endl;
        auto response = HttpClient::get(api_url, headers);
        
        if (response.status_code != 200) {
            throw std::runtime_error(
                "Failed to fetch model info from Hugging Face API (status: " + 
                std::to_string(response.status_code) + ")"
            );
        }
        
        auto model_info = JsonUtils::parse(response.body);
        
        if (!model_info.contains("siblings") || !model_info["siblings"].is_array()) {
            throw std::runtime_error("Invalid model info response from Hugging Face API");
        }
        
        // Extract list of all files in the repository
        std::vector<std::string> repo_files;
        for (const auto& file : model_info["siblings"]) {
            if (file.contains("rfilename")) {
                repo_files.push_back(file["rfilename"].get<std::string>());
            }
        }
        
        std::cout << "[ModelManager] Repository contains " << repo_files.size() << " files" << std::endl;
        
        std::vector<std::string> files_to_download;
        
        // Check if this is a GGUF model (variant provided) or non-GGUF (variant empty)
        if (!variant.empty() || !mmproj.empty()) {
            // GGUF model: Use identify_gguf_models to determine which files to download
            GGUFFiles gguf_files = identify_gguf_models(repo_id, variant, mmproj, repo_files);
            
            // Combine core files and sharded files into one list
            for (const auto& [key, filename] : gguf_files.core_files) {
                files_to_download.push_back(filename);
            }
            for (const auto& filename : gguf_files.sharded_files) {
                files_to_download.push_back(filename);
            }
            
            // Also download essential config files if they exist
            std::vector<std::string> config_files = {
                "config.json",
                "tokenizer.json",
                "tokenizer_config.json",
                "tokenizer.model"
            };
            for (const auto& config_file : config_files) {
                if (std::find(repo_files.begin(), repo_files.end(), config_file) != repo_files.end()) {
                    if (std::find(files_to_download.begin(), files_to_download.end(), config_file) == files_to_download.end()) {
                        files_to_download.push_back(config_file);
                    }
                }
            }
        } else {
            // Non-GGUF model (ONNX, etc.): Download all files in repository
            files_to_download = repo_files;
        }
        
        std::cout << "[ModelManager] Identified " << files_to_download.size() << " files to download:" << std::endl;
        for (const auto& filename : files_to_download) {
            std::cout << "  - " << filename << std::endl;
        }
        
        // Download each file
        for (const auto& filename : files_to_download) {
            std::string file_url = "https://huggingface.co/" + repo_id + 
                                 "/resolve/main/" + filename;
            std::string output_path = snapshot_path + "/" + filename;
            
            // Skip if file already exists
            if (fs::exists(output_path)) {
                std::cout << "[ModelManager] Skipping (already exists): " << filename << std::endl;
                continue;
            }
            
            // Create parent directory for file (handles folders in filenames)
            fs::create_directories(fs::path(output_path).parent_path());
            
            std::cout << "[ModelManager] Downloading: " << filename << "..." << std::endl;
            
            // Download with throttled progress updates (once per second)
            bool success = HttpClient::download_file(
                file_url,
                output_path,
                utils::create_throttled_progress_callback(),
                headers
            );
            
            if (success) {
                std::cout << "\n[ModelManager] Downloaded: " << filename << std::endl;
            } else {
                throw std::runtime_error("Failed to download file: " + filename);
            }
        }
        
        // Validate all expected files exist after download
        std::cout << "[ModelManager] Validating downloaded files..." << std::endl;
        for (const auto& filename : files_to_download) {
            std::string expected_path = snapshot_path + "/" + filename;
            if (!fs::exists(expected_path)) {
                throw std::runtime_error(
                    "Hugging Face snapshot download expected file " + filename + 
                    " not found at " + expected_path
                );
            }
        }
        
        std::cout << "[ModelManager] ✓ All files downloaded and validated successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        // Don't log here - let the caller (Server) handle error logging
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

