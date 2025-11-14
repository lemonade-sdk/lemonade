#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

struct ModelInfo {
    std::string model_name;
    std::string checkpoint;      // Original checkpoint identifier (for downloads/display)
    std::string resolved_path;   // Absolute path to model file/directory on disk
    std::string recipe;
    std::vector<std::string> labels;
    bool suggested = false;
    std::string mmproj;
    std::string source;  // "local_upload" for locally uploaded models
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
                            const std::string& mmproj = "",
                            const std::string& source = "");
    
    // Download a model
    void download_model(const std::string& model_name,
                       const std::string& checkpoint = "",
                       const std::string& recipe = "",
                       bool reasoning = false,
                       bool vision = false,
                       const std::string& mmproj = "",
                       bool do_not_upgrade = false);
    
    // Delete a model
    void delete_model(const std::string& model_name);
    
    // Get model info by name
    ModelInfo get_model_info(const std::string& model_name);
    
    // Check if model exists
    bool model_exists(const std::string& model_name);
    
    // Check if model is downloaded
    bool is_model_downloaded(const std::string& model_name);
    
    // Check if model is downloaded with optional FLM cache (optimization)
    bool is_model_downloaded(const std::string& model_name, 
                             const std::vector<std::string>* flm_cache);
    
    // Get list of installed FLM models (for caching)
    std::vector<std::string> get_flm_installed_models();
    
    // Get HuggingFace cache directory (respects HF_HUB_CACHE, HF_HOME, and platform defaults)
    std::string get_hf_cache_dir() const;
    
private:
    json load_server_models();
    json load_user_models();
    void save_user_models(const json& user_models);
    
    std::string get_cache_dir();
    std::string get_user_models_file();
    
    // Cache management for downloaded models
    void initialize_cache();
    void add_to_cache(const std::string& model_name);
    void remove_from_cache(const std::string& model_name);
    
    // Resolve model checkpoint to absolute path on disk
    std::string resolve_model_path(const ModelInfo& info) const;
    
    // Download from Hugging Face
    void download_from_huggingface(const std::string& repo_id, 
                                   const std::string& variant = "",
                                   const std::string& mmproj = "");
    
    // Download from FLM
    void download_from_flm(const std::string& checkpoint, bool do_not_upgrade = true);
    
    json server_models_;
    json user_models_;
    
    // Cache for downloaded models (avoids disk checks on every request)
    std::mutex downloaded_cache_mutex_;
    std::map<std::string, ModelInfo> downloaded_cache_;
    bool cache_initialized_ = false;
};

} // namespace lemon

