#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>
#include "model_types.h"
#include "recipe_options.h"

namespace lemon {

using json = nlohmann::json;

// Progress information for download operations
struct DownloadProgress {
    std::string file;           // Current file being downloaded
    int file_index = 0;         // Current file index (1-based)
    int total_files = 0;        // Total number of files to download
    size_t bytes_downloaded = 0; // Bytes downloaded for current file
    size_t bytes_total = 0;     // Total bytes for current file
    int percent = 0;            // Overall percentage (0-100)
    bool complete = false;      // True when all downloads finished
    std::string error;          // Error message if failed
};

// Callback for download progress updates
// Returns bool: true = continue download, false = cancel download
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;

// Image generation defaults for SD models
struct ImageDefaults {
    int steps = 20;
    float cfg_scale = 7.0f;
    int width = 512;
    int height = 512;

    bool has_defaults = false;  // True if explicit defaults were provided in JSON
};

struct ModelInfo {
    std::string model_name;
    std::map<std::string, std::string> checkpoints;
    std::map<std::string, std::string> resolved_paths; // Absolute path to model file/directory on disk
    std::string recipe;
    std::vector<std::string> labels;
    bool suggested = false;
    std::string source;  // "local_upload" for locally uploaded models
    bool downloaded = false;     // Whether model is downloaded and available
    double size = 0.0;   // Model size in GB
    RecipeOptions recipe_options;

    // Multi-model support fields
    ModelType type = ModelType::LLM;      // Model type for LRU cache management
    DeviceType device = DEVICE_NONE;      // Target device(s) for this model

    // Image generation defaults (for sd-cpp models)
    ImageDefaults image_defaults;

    // Utility
    std::string checkpoint(const std::string& type = "main") const { return checkpoints.count(type) ? checkpoints.at(type) : ""; }
    std::string resolved_path(const std::string& type = "main") const { return resolved_paths.count(type) ? resolved_paths.at(type) : ""; }

    std::string mmproj() const { return checkpoint("mmproj"); }
};

class ModelManager {
public:
    ModelManager();

    // Get all supported models from server_models.json
    std::map<std::string, ModelInfo> get_supported_models();

    // Get downloaded models
    std::map<std::string, ModelInfo> get_downloaded_models();

    // Filter models by available backends
    std::map<std::string, ModelInfo> filter_models_by_backend(
        const std::map<std::string, ModelInfo>& models);

    // Register a user model
    void register_user_model(const std::string& model_name,
                            const std::string& checkpoint,
                            const std::string& recipe,
                            bool reasoning = false,
                            bool vision = false,
                            bool embedding = false,
                            bool reranking = false,
                            bool image = false,
                            const std::string& mmproj = "",
                            const std::string& source = "");

    // Register (if needed) and download a model
    void download_model(const std::string& model_name,
                       const std::string& checkpoint = "",
                       const std::string& recipe = "",
                       bool reasoning = false,
                       bool vision = false,
                       bool embedding = false,
                       bool reranking = false,
                       bool image = false,
                       const std::string& mmproj = "",
                       bool do_not_upgrade = false,
                       DownloadProgressCallback progress_callback = nullptr);

    // Download a model
    void download_registered_model(const ModelInfo& info,
                                bool do_not_upgrade = false,
                                DownloadProgressCallback progress_callback = nullptr);

    // Delete a model
    void delete_model(const std::string& model_name);

    // Get model info by name
    ModelInfo get_model_info(const std::string& model_name);

    // Check if model exists (in filtered list based on system capabilities)
    bool model_exists(const std::string& model_name);

    // Check if model exists in the raw registry (before filtering)
    // Returns true even for NPU models on systems without NPU
    bool model_exists_unfiltered(const std::string& model_name);

    // Get model info from raw registry (without filtering)
    // Useful for generating helpful error messages about unsupported models
    ModelInfo get_model_info_unfiltered(const std::string& model_name);

    // Get the reason why a model was filtered out (empty string if not filtered)
    // Returns a user-friendly message explaining why the model is not available
    std::string get_model_filter_reason(const std::string& model_name);

    // Check if model is downloaded
    bool is_model_downloaded(const std::string& model_name);

    // Get list of installed FLM models (for caching)
    std::vector<std::string> get_flm_installed_models();

    // Refresh FLM model download status from 'flm list' (call after FLM install/upgrade)
    void refresh_flm_download_status();

    // Get HuggingFace cache directory (respects HF_HUB_CACHE, HF_HOME, and platform defaults)
    std::string get_hf_cache_dir() const;

    // Set extra models directory for GGUF discovery
    void set_extra_models_dir(const std::string& dir);

    void save_model_options(const ModelInfo& info);

private:
    json load_server_models();
    json load_optional_json(const std::string& path);
    void save_user_models(const json& user_models);

    std::string get_user_models_file();
    std::string get_recipe_options_file();

    // Cache management
    void build_cache();
    void add_model_to_cache(const std::string& model_name);
    void update_model_options_in_cache(const ModelInfo& info);
    void update_model_in_cache(const std::string& model_name, bool downloaded);
    void remove_model_from_cache(const std::string& model_name);

    // Resolve model checkpoint to absolute path on disk
    std::string resolve_model_path(const ModelInfo& info, const std::string& type, const std::string& checkpoint) const;
    void resolve_all_model_paths(ModelInfo& info);

    // Download from a JSON manifest
    void download_from_manifest(const json& manifest, std::map<std::string, std::string>& headers, DownloadProgressCallback progress_callback);

    // Download from Hugging Face
    void download_from_huggingface(const ModelInfo& info,
                                   DownloadProgressCallback progress_callback = nullptr);

    // Download from FLM
    void download_from_flm(const std::string& checkpoint,
                          bool do_not_upgrade = true,
                          DownloadProgressCallback progress_callback = nullptr);

    // Discover GGUF models from extra_models_dir
    std::map<std::string, ModelInfo> discover_extra_models() const;

    json server_models_;
    json user_models_;
    json recipe_options_;
    std::string extra_models_dir_;  // Secondary directory for GGUF model discovery

    // Cache of all models with their download status
    mutable std::mutex models_cache_mutex_;
    mutable std::map<std::string, ModelInfo> models_cache_;
    mutable std::map<std::string, std::string> filtered_out_models_;  // model_name -> filter reason
    mutable bool cache_valid_ = false;
};

} // namespace lemon
