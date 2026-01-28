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
    // (case 1) If variant ends in .gguf or .bin, use it directly
    else if (!variant.empty() && (ends_with_ignore_case(variant, ".gguf") || ends_with_ignore_case(variant, ".bin"))) {
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
    user_models_ = load_optional_json(get_user_models_file());
    recipe_options_ = load_optional_json(get_recipe_options_file());
}

std::string ModelManager::get_user_models_file() {
    return get_cache_dir() + "/user_models.json";
}

std::string ModelManager::get_recipe_options_file() {
    return get_cache_dir() + "/recipe_options.json";
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

void ModelManager::set_extra_models_dir(const std::string& dir) {
    extra_models_dir_ = dir;

    // Invalidate cache so discovered models are included on next access
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    cache_valid_ = false;

    if (!extra_models_dir_.empty()) {
        std::cout << "[ModelManager] Extra models directory set to: " << extra_models_dir_ << std::endl;
    }
}

std::map<std::string, ModelInfo> ModelManager::discover_extra_models() const {
    std::map<std::string, ModelInfo> discovered;

    // If no extra models directory configured, return empty
    if (extra_models_dir_.empty()) {
        return discovered;
    }

    if (!fs::exists(extra_models_dir_)) {
        // Directory doesn't exist, return empty
        return discovered;
    }

    std::string search_dir = extra_models_dir_;

    std::cout << "[ModelManager] Scanning for GGUF models in: " << search_dir << std::endl;

    // Configuration for discovered models (single source of truth)
    static constexpr const char* EXTRA_MODEL_PREFIX = "extra.";
    static constexpr const char* EXTRA_MODEL_RECIPE = "llamacpp";
    static constexpr const char* EXTRA_MODEL_SOURCE = "extra_models_dir";

    // Helper to initialize common ModelInfo fields for discovered models
    auto init_extra_model_info = [this](const std::string& name) -> ModelInfo {
        ModelInfo info;
        info.model_name = name;
        info.recipe = EXTRA_MODEL_RECIPE;
        info.suggested = true;
        info.downloaded = true;
        info.source = EXTRA_MODEL_SOURCE;
        info.labels.push_back("custom");
        info.device = get_device_type_from_recipe(EXTRA_MODEL_RECIPE);
        return info;
    };

    // Track which directories we've processed (for multimodal/multi-shard detection)
    std::map<std::string, std::vector<fs::path>> dirs_with_gguf;  // directory -> list of gguf files
    std::vector<fs::path> standalone_files;  // GGUF files not in subdirectories

    // Recursively find all .gguf files
    try {
        for (const auto& entry : fs::recursive_directory_iterator(search_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();

            if (!ends_with_ignore_case(filename, ".gguf")) continue;

            fs::path parent_dir = entry.path().parent_path();

            // Check if this file is directly in the search directory or in a subdirectory
            if (parent_dir == fs::path(search_dir)) {
                // Standalone file in the root of search directory
                standalone_files.push_back(entry.path());
            } else {
                // File in a subdirectory - group by parent directory
                dirs_with_gguf[parent_dir.string()].push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ModelManager] Error scanning directory " << search_dir << ": " << e.what() << std::endl;
        return discovered;
    }

    // Process standalone files (single-file models)
    for (const auto& gguf_path : standalone_files) {
        std::string filename = gguf_path.filename().string();

        // Skip mmproj files - they're part of multimodal models
        if (contains_ignore_case(filename, "mmproj")) continue;

        std::string model_name = std::string(EXTRA_MODEL_PREFIX) + filename;
        ModelInfo info = init_extra_model_info(model_name);
        info.checkpoint = gguf_path.string();
        info.resolved_path = gguf_path.string();
        info.type = ModelType::LLM;

        // Calculate size in GB
        try {
            uintmax_t file_size = fs::file_size(gguf_path);
            info.size = static_cast<double>(file_size) / (1024.0 * 1024.0 * 1024.0);
        } catch (...) {
            info.size = 0.0;
        }

        discovered[model_name] = info;
    }

    // Process directories (multimodal and multi-shard models)
    for (const auto& [dir_path, gguf_files] : dirs_with_gguf) {
        if (gguf_files.empty()) continue;

        fs::path dir = fs::path(dir_path);
        std::string dir_name = dir.filename().string();

        // Find the main model file and mmproj file
        fs::path main_model_path;
        std::string mmproj_file;
        double total_size = 0.0;

        for (const auto& gguf_path : gguf_files) {
            std::string filename = gguf_path.filename().string();

            // Calculate total size
            try {
                uintmax_t file_size = fs::file_size(gguf_path);
                total_size += static_cast<double>(file_size) / (1024.0 * 1024.0 * 1024.0);
            } catch (...) {}

            // Check if this is an mmproj file (can be anywhere in filename)
            if (contains_ignore_case(filename, "mmproj")) {
                mmproj_file = filename;
                continue;
            }

            // This is a model file - for sharded models, we want the first shard
            // For non-sharded, this is the only model file
            if (main_model_path.empty() || gguf_path < main_model_path) {
                main_model_path = gguf_path;
            }
        }

        if (main_model_path.empty()) {
            // No main model file found (only mmproj?), skip
            continue;
        }

        std::string model_name = std::string(EXTRA_MODEL_PREFIX) + dir_name;
        ModelInfo info = init_extra_model_info(model_name);
        info.checkpoint = dir_path;
        info.resolved_path = main_model_path.string();
        info.size = total_size;

        // If mmproj found, set it and add vision label
        if (!mmproj_file.empty()) {
            info.mmproj = mmproj_file;
            info.labels.push_back("vision");
        }

        info.type = get_model_type_from_labels(info.labels);

        discovered[model_name] = info;
    }

    std::cout << "[ModelManager] Discovered " << discovered.size() << " models from extra directory" << std::endl;

    return discovered;
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

json ModelManager::load_optional_json(const std::string& path) {
    if (!fs::exists(path)) {
        return json::object();
    }

    try {
        std::cout << "[ModelManager] Loading " << fs::path(path).filename() << std::endl;
        return JsonUtils::load_from_file(path);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not load " << fs::path(path).filename() << ": " << e.what() << std::endl;
        return json::object();
    }
}

static void save_user_json(const std::string& save_path, const json& to_save) {
    // Ensure directory exists
    fs::path dir = fs::path(save_path).parent_path();
    fs::create_directories(dir);

    std::cout << "[ModelManager] Saving " << fs::path(save_path).filename() << std::endl;
    JsonUtils::save_to_file(to_save, save_path);
}

void ModelManager::save_user_models(const json& user_models) {
    save_user_json(get_user_models_file(), user_models);
}

void ModelManager::save_model_options(const ModelInfo& info) {
    std::cout << "[ModelManager] Saving options for model: " << info.model_name << std::endl;
    // Persist changes
    recipe_options_[info.model_name] = info.recipe_options.to_json();
    update_model_options_in_cache(info);
    save_user_json(get_recipe_options_file(), recipe_options_);
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

        // Parse image_defaults if present (for sd-cpp models)
        if (value.contains("image_defaults") && value["image_defaults"].is_object()) {
            const auto& img_defaults = value["image_defaults"];
            info.image_defaults.has_defaults = true;
            info.image_defaults.steps = JsonUtils::get_or_default<int>(img_defaults, "steps", 20);
            info.image_defaults.cfg_scale = JsonUtils::get_or_default<float>(img_defaults, "cfg_scale", 7.0f);
            info.image_defaults.width = JsonUtils::get_or_default<int>(img_defaults, "width", 512);
            info.image_defaults.height = JsonUtils::get_or_default<int>(img_defaults, "height", 512);
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

        // Parse image_defaults if present (for sd-cpp models)
        if (value.contains("image_defaults") && value["image_defaults"].is_object()) {
            const auto& img_defaults = value["image_defaults"];
            info.image_defaults.has_defaults = true;
            info.image_defaults.steps = JsonUtils::get_or_default<int>(img_defaults, "steps", 20);
            info.image_defaults.cfg_scale = JsonUtils::get_or_default<float>(img_defaults, "cfg_scale", 7.0f);
            info.image_defaults.width = JsonUtils::get_or_default<int>(img_defaults, "width", 512);
            info.image_defaults.height = JsonUtils::get_or_default<int>(img_defaults, "height", 512);
        }

        // Populate type and device fields (multi-model support)
        info.type = get_model_type_from_labels(info.labels);
        info.device = get_device_type_from_recipe(info.recipe);

        info.resolved_path = resolve_model_path(info);
        all_models[info.model_name] = info;
    }

    // Step 1.5: Discover models from extra_models_dir
    // All discovered models are prefixed with "extra." to avoid conflicts
    // We still gracefully handle conflicts just in case, though
    auto discovered_models = discover_extra_models();
    for (const auto& [name, info] : discovered_models) {
        // Check for conflicts with registered models
        if (all_models.find(name) != all_models.end()) {
            std::cout << "[ModelManager] Warning: Discovered model '" << name
                      << "' conflicts with registered model, skipping." << std::endl;
            continue;
        }
        all_models[name] = info;
    }

    // Populate recipe options
    for (auto& [name, info] : all_models) {
        // Start with image_defaults as base options for sd-cpp models
        json base_options = json::object();
        if (info.image_defaults.has_defaults) {
            base_options["steps"] = info.image_defaults.steps;
            base_options["cfg_scale"] = info.image_defaults.cfg_scale;
            base_options["width"] = info.image_defaults.width;
            base_options["height"] = info.image_defaults.height;
        }

        // User-saved recipe options override image_defaults
        if (JsonUtils::has_key(recipe_options_, name)) {
            std::cout << "[ModelManager] Found recipe options for model: " << name << std::endl;
            auto saved_options = recipe_options_[name];
            // Merge saved options over base options
            for (auto& [key, value] : saved_options.items()) {
                base_options[key] = value;
            }
        }

        info.recipe_options = RecipeOptions(info.recipe, base_options);
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
    info.recipe_options = RecipeOptions(info.recipe, JsonUtils::get_or_default(recipe_options_, model_name, json::object()));
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

void ModelManager::update_model_options_in_cache(const ModelInfo& info) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return; // Will rebuild on next access
    }

    auto it = models_cache_.find(info.model_name);
    if (it != models_cache_.end()) {
        it->second.recipe_options = info.recipe_options;
    } else {
        std::cerr << "[ModelManager] Warning: '" << info.model_name << "' not found in cache" << std::endl;
    }
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
        // User models and local uploads should be removed entirely from cache
        // (they're not in server_models.json, so keeping them makes no sense)
        bool is_user_model = model_name.substr(0, 5) == "user.";
        if (is_user_model || it->second.source == "local_upload") {
            models_cache_.erase(model_name);
            std::cout << "[ModelManager] Removed '" << model_name << "' from cache" << std::endl;
        } else {
            // Registered model - just mark as not downloaded
            it->second.downloaded = false;
            std::cout << "[ModelManager] Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}

void ModelManager::refresh_flm_download_status() {
    // Get fresh list of installed FLM models
    // This is called on every get_supported_models() to ensure FLM status is up-to-date
    // since users can install/uninstall FLM models outside of lemonade
    auto flm_models = get_flm_installed_models();
    std::unordered_set<std::string> flm_set(flm_models.begin(), flm_models.end());

    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return;  // Cache will be built fresh on next access
    }

    // Update download status for all FLM models in cache
    for (auto& [name, info] : models_cache_) {
        if (info.recipe == "flm") {
            bool was_downloaded = info.downloaded;
            info.downloaded = flm_set.count(info.checkpoint) > 0;

            // Log changes for debugging
            if (was_downloaded != info.downloaded) {
                std::cout << "[ModelManager] FLM status changed: " << name
                          << " (checkpoint: " << info.checkpoint << ") -> "
                          << (info.downloaded ? "downloaded" : "not downloaded") << std::endl;
            }
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



double get_max_memory_of_device(json device, MemoryAllocBehavior mem_alloc_behavior) {
    // Get the maximum POSSIBLE accessible memory of the device in question,
    // taking into account the respective memory allocation behavior.

    double virtual_mem_gb = 0.0;
    double vram_gb = 0.0;

    if (device.contains("vram_gb")) {
        vram_gb = device["vram_gb"].get<double>();
    }
    if (device.contains("dynamic_mem_gb"))
    {
        virtual_mem_gb = device["dynamic_mem_gb"].get<double>();
    }

    switch (mem_alloc_behavior)
    {
    case MemoryAllocBehavior::Hardware:
        return vram_gb;

    case MemoryAllocBehavior::Virtual:
        return virtual_mem_gb;

    case MemoryAllocBehavior::Largest:
        return vram_gb > virtual_mem_gb ? vram_gb : virtual_mem_gb;

    case MemoryAllocBehavior::Unified:
        return virtual_mem_gb + vram_gb;

    default:
        return vram_gb;
    }
}

bool parse_TF_env_var(const char* env_var_name) {
    const char* env = std::getenv(env_var_name);
    return env && (std::string(env) == "1" ||
                   std::string(env) == "true" ||
                   std::string(env) == "TRUE" ||
                   std::string(env) == "yes");
}

std::map<std::string, ModelInfo> ModelManager::filter_models_by_backend(
    const std::map<std::string, ModelInfo>& models) {

    // Check if model filtering is disabled via environment variable
    bool disable_filtering = parse_TF_env_var("LEMONADE_DISABLE_MODEL_FILTERING");

    // Check if dGPUs should use GTT
    bool enable_dgpu_gtt = parse_TF_env_var("LEMONADE_ENABLE_DGPU_GTT");

    if (disable_filtering) {
        filtered_out_models_.clear();
        return models;
    }

    if (enable_dgpu_gtt)
    {
      std::cout << "[ModelManager]: LEMONADE_ENABLE_DGPU_GTT has been set to true." << std::endl
                << "     Models are being filtered assuming GTT memory." << std::endl
                << "     Using GTT on a dGPU will have a significant performance impact." << std::endl;
    }

    std::map<std::string, ModelInfo> filtered;

    // Clear the filtered-out models cache (will be repopulated below)
    filtered_out_models_.clear();

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

    // Get largest VRAM object for memory-based filtering
    double largest_mem_pool_gb = 0.0;
    double curr_mem_pool_gb = 0.0;

    for (const auto& [dev_type, devices] : hardware.items()) {
        // Because we have mixed types this just makes every device_type an array.
        nlohmann::json dev_list = devices.is_array() ? devices : nlohmann::json{devices};

        // Expand this later to accommodate mixed pools
        MemoryAllocBehavior dev_mem_alloc_behavior = MemoryAllocBehavior::Hardware;
        if (dev_type == "amd_igpu")
            dev_mem_alloc_behavior = MemoryAllocBehavior::Largest;
        if (enable_dgpu_gtt)
            dev_mem_alloc_behavior = MemoryAllocBehavior::Unified;

        for (const auto& dev : dev_list) {
            curr_mem_pool_gb = get_max_memory_of_device(dev, dev_mem_alloc_behavior);
            largest_mem_pool_gb = largest_mem_pool_gb < curr_mem_pool_gb ? curr_mem_pool_gb : largest_mem_pool_gb;
        }
    }

    // Get system RAM for memory-based filtering
    double system_ram_gb = 0.0;
    if (system_info.contains("Physical Memory") && system_info["Physical Memory"].is_string()) {
        system_ram_gb = parse_physical_memory_gb(system_info["Physical Memory"].get<std::string>());
    }

    // Use the largest of the two values
    double max_model_size_gb = largest_mem_pool_gb > (system_ram_gb * 0.8) ? largest_mem_pool_gb : (system_ram_gb * 0.8);

    // Get processor and OS for user-friendly error messages
    std::string processor = "Unknown";
    std::string os_version = "Unknown";
    if (system_info.contains("Processor") && system_info["Processor"].is_string()) {
        processor = system_info["Processor"].get<std::string>();
    }
    if (system_info.contains("OS Version") && system_info["OS Version"].is_string()) {
        os_version = system_info["OS Version"].get<std::string>();
    }

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
        if (largest_mem_pool_gb > 0.0) {
            std::cout << "  - Largest memory pool: " << std::fixed << std::setprecision(1) << largest_mem_pool_gb << std::endl;
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
                filter_reason = "NPU models require AMD Ryzen AI 300- and 400-series processors with XDNA2 NPUs running Windows 11. "
                               "Detected processor: " + processor + ". "
                               "Detected operating system: " + os_version + ".";
            }
        }

        // Filter OGA models based on NPU availability
        if (recipe == "oga-npu" || recipe == "oga-hybrid") {
            if (!oga_available) {
                filter_out = true;
                filter_reason = "NPU models require AMD Ryzen AI 300- and 400-series processors with XDNA2 NPUs running Windows 11. "
                               "Detected processor: " + processor + ". "
                               "Detected operating system: " + os_version + ".";
            }
        }

        // OGA-CPU models
        if (recipe == "oga-cpu" && !oga_available) {
            filter_out = true;
            filter_reason = "OGA-CPU models require AMD Ryzen AI 300- and 400-series processors running Windows 11. "
                           "Detected processor: " + processor + ". "
                           "Detected operating system: " + os_version + ".";
        }

        // Filter out other OGA models (not yet implemented)
        if (recipe == "oga-igpu") {
            filter_out = true;
            filter_reason = "The oga-igpu recipe is not yet implemented in this version of Lemonade Server.";
        }

        // On macOS, only show llamacpp models
        if (is_macos && recipe != "llamacpp") {
            filter_out = true;
            filter_reason = "This model uses the '" + recipe + "' recipe which is not supported on macOS. "
                           "Only llamacpp models are supported on macOS.";
        }

        // Filter out models that are too large for system RAM
        // Heuristic: if model size > 80% of system RAM, filter it out
        if (!filter_out && system_ram_gb > 0.0 && info.size > 0.0) {
            if (info.size > max_model_size_gb) {
                filter_out = true;
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1);
                oss << "This model requires approximately " << info.size << " GB of memory, "
                    << "but your system only has " << system_ram_gb << " GB of RAM. "
                    << "Models larger than " << max_model_size_gb << " GB (80% of system RAM) are filtered out.";
                filter_reason = oss.str();
            }
        }

        // Special rule: filter out gpt-oss-20b-FLM on systems with less than 64 GB RAM
        if (!filter_out && name == "gpt-oss-20b-FLM" && system_ram_gb > 0.0 && system_ram_gb < 64.0) {
            filter_out = true;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1);
            oss << "The gpt-oss-20b-FLM model requires at least 64 GB of RAM. "
                << "Your system has " << system_ram_gb << " GB.";
            filter_reason = oss.str();
        }

        if (filter_out) {
            filtered_count++;
            // Store the filter reason for later lookup
            filtered_out_models_[name] = filter_reason;
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
                                      bool image,
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
    if (image) {
        labels.push_back("image");
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

void ModelManager::download_model(const std::string& model_name,
                                 const std::string& checkpoint,
                                 const std::string& recipe,
                                 bool reasoning,
                                 bool vision,
                                 bool embedding,
                                 bool reranking,
                                 bool image,
                                 const std::string& mmproj,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback) {

    std::string actual_checkpoint = checkpoint;
    std::string actual_recipe = recipe;
    std::string actual_mmproj = mmproj;

    // If checkpoint or recipe are provided, this is a model registration
    // and the model name must have the "user." prefix
    if (!actual_checkpoint.empty() || !actual_recipe.empty()) {
        if (model_name.substr(0, 5) != "user.") {
            throw std::runtime_error(
                "When providing 'checkpoint' or 'recipe', the model name must include the "
                "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }
    }

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

        // Also look up mmproj if not provided (for vision models)
        if (actual_mmproj.empty()) {
            auto info = get_model_info(model_name);
            actual_mmproj = info.mmproj;
            if (!actual_mmproj.empty()) {
                std::cout << "[ModelManager] Found mmproj for vision model: " << actual_mmproj << std::endl;
            }
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
        download_from_flm(actual_checkpoint, do_not_upgrade, progress_callback);
    } else if (actual_recipe == "llamacpp" || actual_recipe == "whispercpp" || actual_recipe == "sd-cpp") {
        // For llamacpp (GGUF), whispercpp (.bin), and sd-cpp (.safetensors) models, use variant-aware download
        download_from_huggingface(repo_id, variant, actual_mmproj, progress_callback);
    } else {
        // For non-GGUF models (oga-*, etc.), download all files (no variant filtering)
        download_from_huggingface(repo_id, "", "", progress_callback);
    }

    // Register user models to user_models.json
    if (model_name.substr(0, 5) == "user.") {
        register_user_model(model_name, actual_checkpoint, actual_recipe,
                          reasoning, vision, embedding, reranking, image, actual_mmproj);
    }

    // Update cache after successful download
    update_model_in_cache(model_name, true);
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
                                            const std::string& mmproj,
                                            DownloadProgressCallback progress_callback) {
    // Get Hugging Face cache directory
    std::string hf_cache = get_hf_cache_dir();

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

        // Extract commit hash (sha) from the API response
        std::string commit_hash;
        if (model_info.contains("sha") && model_info["sha"].is_string()) {
            commit_hash = model_info["sha"].get<std::string>();
            std::cout << "[ModelManager] Using commit hash: " << commit_hash << std::endl;
        } else {
            // Fallback to "main" if sha is not available
            commit_hash = "main";
            std::cout << "[ModelManager] Warning: No commit hash found in API response, using 'main'" << std::endl;
        }

        // Create snapshot directory using commit hash
        std::string snapshot_path = model_cache_path + "/snapshots/" + commit_hash;
        fs::create_directories(snapshot_path);

        // Create refs/main file pointing to this commit (matching huggingface_hub behavior)
        std::string refs_dir = model_cache_path + "/refs";
        fs::create_directories(refs_dir);
        std::string refs_main_path = refs_dir + "/main";
        std::ofstream refs_file(refs_main_path);
        if (refs_file.is_open()) {
            refs_file << commit_hash;
            refs_file.close();
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
            // Check if variant is a safetensors file (for sd-cpp models)
            bool is_safetensors = variant.size() > 12 &&
                variant.substr(variant.size() - 12) == ".safetensors";

            if (is_safetensors) {
                // For safetensors files, just download the specified file directly
                if (std::find(repo_files.begin(), repo_files.end(), variant) != repo_files.end()) {
                    files_to_download.push_back(variant);
                    std::cout << "[ModelManager] Found safetensors file: " << variant << std::endl;
                } else {
                    throw std::runtime_error("Safetensors file not found in repository: " + variant);
                }
            } else {
                // GGUF model: Use identify_gguf_models to determine which files to download
                GGUFFiles gguf_files = identify_gguf_models(repo_id, variant, mmproj, repo_files);

                // Combine core files and sharded files into one list
                for (const auto& [key, filename] : gguf_files.core_files) {
                    files_to_download.push_back(filename);
                }
                for (const auto& filename : gguf_files.sharded_files) {
                    files_to_download.push_back(filename);
                }
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

        // Create download manifest to track incomplete downloads
        // This allows us to detect partially downloaded models
        std::string manifest_path = snapshot_path + "/.download_manifest.json";

        // Fetch file sizes from the tree API (the models API doesn't include sizes)
        std::map<std::string, size_t> file_sizes;
        std::string tree_url = "https://huggingface.co/api/models/" + repo_id + "/tree/main";
        auto tree_response = HttpClient::get(tree_url, headers);

        if (tree_response.status_code == 200) {
            auto tree_info = JsonUtils::parse(tree_response.body);
            if (tree_info.is_array()) {
                for (const auto& file : tree_info) {
                    if (file.contains("path") && file.contains("size")) {
                        std::string fpath = file["path"].get<std::string>();
                        size_t fsize = file["size"].get<size_t>();
                        file_sizes[fpath] = fsize;
                    }
                }
            }
            std::cout << "[ModelManager] Retrieved file sizes for " << file_sizes.size() << " files" << std::endl;
        } else {
            std::cout << "[ModelManager] Warning: Could not fetch file sizes (tree API returned "
                      << tree_response.status_code << ")" << std::endl;
        }

        // Create manifest with expected files
        json manifest;
        manifest["repo_id"] = repo_id;
        manifest["commit_hash"] = commit_hash;
        manifest["files"] = json::array();
        for (const auto& filename : files_to_download) {
            json file_entry;
            file_entry["name"] = filename;
            file_entry["size"] = file_sizes.count(filename) ? file_sizes[filename] : 0;
            manifest["files"].push_back(file_entry);
        }

        // Write manifest (indicates download in progress)
        JsonUtils::save_to_file(manifest, manifest_path);
        std::cout << "[ModelManager] Created download manifest" << std::endl;

        // Download each file with robust retry and resume support
        int file_index = 0;
        int total_files = static_cast<int>(files_to_download.size());

        for (const auto& filename : files_to_download) {
            file_index++;
            std::string file_url = "https://huggingface.co/" + repo_id +
                                 "/resolve/main/" + filename;
            std::string output_path = snapshot_path + "/" + filename;

            // Create parent directory for file (handles folders in filenames)
            fs::create_directories(fs::path(output_path).parent_path());

            std::cout << "[ModelManager] Downloading: " << filename << "..." << std::endl;

            // Send progress update if callback provided (and check for cancellation)
            if (progress_callback) {
                DownloadProgress progress;
                progress.file = filename;
                progress.file_index = file_index;
                progress.total_files = total_files;
                progress.bytes_downloaded = 0;
                progress.bytes_total = file_sizes.count(filename) ? file_sizes[filename] : 0;
                progress.percent = 0;
                if (!progress_callback(progress)) {
                    std::cout << "[ModelManager] Download cancelled by client" << std::endl;
                    throw std::runtime_error("Download cancelled");
                }
            }

            utils::DownloadOptions download_opts;
            download_opts.max_retries = 10;
            download_opts.initial_retry_delay_ms = 2000;
            download_opts.max_retry_delay_ms = 120000;
            download_opts.resume_partial = true;
            download_opts.low_speed_limit = 1000;
            download_opts.low_speed_time = 60;
            download_opts.connect_timeout = 60;

            // Create progress callback that reports to both console and SSE callback
            // Returns bool: true = continue, false = cancel
            utils::ProgressCallback http_progress_cb;
            if (progress_callback) {
                http_progress_cb = [&](size_t downloaded, size_t total) -> bool {
                    DownloadProgress progress;
                    progress.file = filename;
                    progress.file_index = file_index;
                    progress.total_files = total_files;
                    progress.bytes_downloaded = downloaded;
                    progress.bytes_total = total;
                    progress.percent = (total > 0) ? static_cast<int>((downloaded * 100) / total) : 0;
                    return progress_callback(progress);  // Propagate cancellation
                };
            } else {
                // Default console progress callback (never cancels)
                http_progress_cb = utils::create_throttled_progress_callback();
            }

            auto result = HttpClient::download_file(
                file_url,
                output_path,
                http_progress_cb,
                headers,
                download_opts
            );

            // Check if download was cancelled
            if (result.cancelled) {
                std::cout << "[ModelManager] Download cancelled by client" << std::endl;
                throw std::runtime_error("Download cancelled");
            }

            if (result.success) {
                std::cout << "\n[ModelManager] Downloaded: " << filename << std::endl;
            } else {
                // Build a detailed error message
                std::ostringstream error_msg;
                error_msg << "Failed to download file: " << filename << "\n";
                error_msg << "URL: " << file_url << "\n";
                error_msg << result.error_message;

                // If there's a partial file, provide helpful information
                if (fs::exists(output_path)) {
                    size_t partial_size = fs::file_size(output_path);
                    if (partial_size > 0) {
                        error_msg << "\n\n[INFO] Partial download preserved at: " << output_path;
                        error_msg << "\n[INFO] Partial size: " << std::fixed << std::setprecision(1)
                                  << (partial_size / (1024.0 * 1024.0)) << " MB";
                        error_msg << "\n[INFO] Run the command again to resume from where it left off.";
                    }
                }

                throw std::runtime_error(error_msg.str());
            }
        }

        // Validate all expected files exist after download
        std::cout << "[ModelManager] Validating downloaded files..." << std::endl;
        bool all_valid = true;
        for (const auto& filename : files_to_download) {
            std::string expected_path = snapshot_path + "/" + filename;
            std::string partial_path = expected_path + ".partial";

            // Check for .partial file (incomplete download)
            if (fs::exists(partial_path)) {
                all_valid = false;
                std::cerr << "[ModelManager] Incomplete file found: " << filename << ".partial" << std::endl;
                continue;
            }

            if (!fs::exists(expected_path)) {
                all_valid = false;
                std::cerr << "[ModelManager] Missing file: " << filename << std::endl;
                continue;
            }

            // Verify file size if we have expected size
            if (file_sizes.count(filename) && file_sizes[filename] > 0) {
                size_t actual_size = fs::file_size(expected_path);
                size_t expected_size = file_sizes[filename];
                if (actual_size != expected_size) {
                    all_valid = false;
                    std::cerr << "[ModelManager] Size mismatch for " << filename
                              << ": expected " << expected_size << " bytes, got " << actual_size << " bytes" << std::endl;
                }
            }
        }

        if (!all_valid) {
            throw std::runtime_error(
                "Download validation failed. Some files are incomplete or missing. "
                "Run the command again to resume."
            );
        }

        // All files validated - remove manifest to mark download as complete
        if (fs::exists(manifest_path)) {
            fs::remove(manifest_path);
            std::cout << "[ModelManager] Removed download manifest (download complete)" << std::endl;
        }

        // Send completion event
        // Note: We ignore the return value here since the download is already complete
        // If the client disconnected, the write will simply fail silently
        if (progress_callback) {
            DownloadProgress progress;
            progress.complete = true;
            progress.file_index = total_files;
            progress.total_files = total_files;
            progress.percent = 100;
            (void)progress_callback(progress);  // Ignore return - download already complete
        }

        std::cout << "[ModelManager] ✓ All files downloaded and validated successfully!" << std::endl;
        std::cout << "[ModelManager DEBUG] Download location: " << snapshot_path << std::endl;

    } catch (const std::exception& e) {
        // Don't log here - let the caller (Server) handle error logging
        throw;
    }
}

void ModelManager::download_from_flm(const std::string& checkpoint,
                                     bool do_not_upgrade,
                                     DownloadProgressCallback progress_callback) {
    std::cout << "[ModelManager] Pulling FLM model: " << checkpoint << std::endl;

    // Ensure FLM is installed
    std::cout << "[ModelManager] Checking FLM installation..." << std::endl;
    backends::FastFlowLMServer flm_installer("info", this);
    try {
        flm_installer.install();
    } catch (const std::exception& e) {
        std::cerr << "[ModelManager ERROR] FLM installation failed: " << e.what() << std::endl;
        throw;
    }

    // Find flm executable
    std::string flm_path = "flm";

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

    // State for parsing FLM output
    int total_files = 0;
    int current_file_index = 0;
    std::string current_filename;
    bool cancelled = false;

    // Run flm pull command and parse output
    int exit_code = utils::ProcessManager::run_process_with_output(
        flm_path, args,
        [&](const std::string& line) -> bool {
            // Always print the line to console
            std::cout << line << std::endl;

            // Parse FLM output to extract progress information
            // Pattern: "[FLM]  Downloading X/Y: filename"
            if (line.find("[FLM]  Downloading ") != std::string::npos &&
                line.find("/") != std::string::npos &&
                line.find(":") != std::string::npos) {

                // Extract "X/Y: filename" from "[FLM]  Downloading X/Y: filename"
                size_t start = line.find("Downloading ") + 12;
                size_t slash = line.find("/", start);
                size_t colon = line.find(":", slash);

                if (slash != std::string::npos && colon != std::string::npos) {
                    try {
                        current_file_index = std::stoi(line.substr(start, slash - start));
                        total_files = std::stoi(line.substr(slash + 1, colon - slash - 1));
                        current_filename = line.substr(colon + 2);  // Skip ": "

                        // Send progress update
                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = 0;
                            progress.bytes_total = 0;
                            progress.percent = (total_files > 0) ?
                                ((current_file_index - 1) * 100 / total_files) : 0;

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Downloading: XX.X% (XXX.XMB / XXX.XMB)"
            else if (line.find("[FLM]  Downloading: ") != std::string::npos &&
                     line.find("%") != std::string::npos) {

                // Extract percentage and bytes
                size_t start = line.find("Downloading: ") + 13;
                size_t pct_end = line.find("%", start);

                if (pct_end != std::string::npos) {
                    try {
                        std::string pct_str = line.substr(start, pct_end - start);
                        double file_percent = std::stod(pct_str);

                        // Try to extract bytes (XXX.XMB / XXX.XMB)
                        size_t open_paren = line.find("(", pct_end);
                        size_t slash = line.find("/", open_paren);
                        size_t close_paren = line.find(")", slash);

                        size_t bytes_downloaded = 0;
                        size_t bytes_total = 0;

                        if (open_paren != std::string::npos && slash != std::string::npos) {
                            std::string downloaded_str = line.substr(open_paren + 1, slash - open_paren - 1);
                            std::string total_str = line.substr(slash + 1, close_paren - slash - 1);

                            // Parse "XXX.XMB" format
                            auto parse_size = [](const std::string& s) -> size_t {
                                double val = 0;
                                size_t mb_pos = s.find("MB");
                                size_t gb_pos = s.find("GB");
                                size_t kb_pos = s.find("KB");

                                if (mb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, mb_pos));
                                    return static_cast<size_t>(val * 1024 * 1024);
                                } else if (gb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, gb_pos));
                                    return static_cast<size_t>(val * 1024 * 1024 * 1024);
                                } else if (kb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, kb_pos));
                                    return static_cast<size_t>(val * 1024);
                                }
                                return 0;
                            };

                            bytes_downloaded = parse_size(downloaded_str);
                            bytes_total = parse_size(total_str);
                        }

                        // Send progress update with byte-level info
                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = bytes_downloaded;
                            progress.bytes_total = bytes_total;
                            // Use intra-file percent when we have byte-level progress
                            progress.percent = static_cast<int>(file_percent);

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Overall progress: XX.X% (X/Y files)"
            else if (line.find("[FLM]  Overall progress: ") != std::string::npos) {
                size_t start = line.find("progress: ") + 10;
                size_t pct_end = line.find("%", start);

                if (pct_end != std::string::npos) {
                    try {
                        int overall_percent = static_cast<int>(std::stod(line.substr(start, pct_end - start)));

                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = 0;  // Not available for overall progress
                            progress.bytes_total = 0;
                            progress.percent = overall_percent;

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Missing files (N):"
            else if (line.find("[FLM]  Missing files (") != std::string::npos) {
                size_t start = line.find("(") + 1;
                size_t end = line.find(")", start);
                if (end != std::string::npos) {
                    try {
                        total_files = std::stoi(line.substr(start, end - start));
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }

            return true;  // Continue
        },
        "",  // Working directory
        3600  // 1 hour timeout for large model downloads
    );

    if (cancelled) {
        std::cout << "[ModelManager] FLM download cancelled by client" << std::endl;
        throw std::runtime_error("Download cancelled");
    }

    if (exit_code != 0) {
        std::cerr << "[ModelManager ERROR] FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
    }

    // Send completion event
    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);  // Ignore return - download already complete
    }

    std::cout << "[ModelManager] FLM model pull completed successfully" << std::endl;
}

void ModelManager::delete_model(const std::string& model_name) {
    auto info = get_model_info(model_name);

    std::cout << "[ModelManager] Deleting model: " << model_name << std::endl;
    std::cout << "[ModelManager] Checkpoint: " << info.checkpoint << std::endl;
    std::cout << "[ModelManager] Recipe: " << info.recipe << std::endl;

    // Handle extra models (from --extra-models-dir) - these are user-managed external files
    if (model_name.substr(0, 6) == "extra.") {
        throw std::runtime_error("Cannot delete extra models via API. Models in --extra-models-dir are user-managed. "
                                 "Delete the file directly from: " + info.checkpoint);
    }

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

bool ModelManager::model_exists_unfiltered(const std::string& model_name) {
    // Check raw server_models_ JSON (before filtering)
    if (server_models_.contains(model_name)) {
        return true;
    }
    // Also check user models
    if (user_models_.contains(model_name)) {
        return true;
    }
    return false;
}

ModelInfo ModelManager::get_model_info_unfiltered(const std::string& model_name) {
    ModelInfo info;

    // Check server models first
    json* model_json = nullptr;
    if (server_models_.contains(model_name)) {
        model_json = &server_models_[model_name];
    } else if (user_models_.contains(model_name)) {
        model_json = &user_models_[model_name];
    }

    if (!model_json) {
        throw std::runtime_error("Model not found in registry: " + model_name);
    }

    // Parse model info from JSON
    info.model_name = model_name;
    info.checkpoint = JsonUtils::get_or_default<std::string>(*model_json, "checkpoint", "");
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", false);
    info.mmproj = JsonUtils::get_or_default<std::string>(*model_json, "mmproj", "");
    info.source = JsonUtils::get_or_default<std::string>(*model_json, "source", "");

    // Parse labels array
    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            if (label.is_string()) {
                info.labels.push_back(label.get<std::string>());
            }
        }
    }

    // Parse size
    if (model_json->contains("size")) {
        if ((*model_json)["size"].is_number()) {
            info.size = (*model_json)["size"].get<double>();
        }
    }

    return info;
}

std::string ModelManager::get_model_filter_reason(const std::string& model_name) {
    // Ensure cache is built (this populates filtered_out_models_)
    build_cache();

    // Look up in the filtered-out models cache
    // This is populated by filter_models_by_backend() during cache building
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    auto it = filtered_out_models_.find(model_name);
    if (it != filtered_out_models_.end()) {
        return it->second;
    }

    // Model wasn't filtered out (either it's available or doesn't exist)
    return "";
}

} // namespace lemon
