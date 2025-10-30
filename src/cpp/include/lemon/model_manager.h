#pragma once

#include <string>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

struct ModelInfo {
    std::string model_name;
    std::string checkpoint;
    std::string recipe;
    std::vector<std::string> labels;
    bool suggested = false;
    std::string mmproj;
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
                            const std::string& mmproj = "");
    
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
    
private:
    json load_server_models();
    json load_user_models();
    void save_user_models(const json& user_models);
    
    std::string get_cache_dir();
    std::string get_user_models_file();
    
    // Download from Hugging Face
    void download_from_huggingface(const std::string& repo_id, 
                                   const std::string& variant = "",
                                   const std::string& mmproj = "");
    
    // Download from FLM
    void download_from_flm(const std::string& checkpoint, bool do_not_upgrade = true);
    
    json server_models_;
    json user_models_;
};

} // namespace lemon

