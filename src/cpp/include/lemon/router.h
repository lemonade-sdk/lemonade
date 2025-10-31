#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include "wrapped_server.h"

namespace lemon {

using json = nlohmann::json;

class Router {
public:
    Router(int ctx_size = 4096, 
           const std::string& llamacpp_backend = "vulkan",
           const std::string& log_level = "info");
    
    ~Router();
    
    // Load a model with the appropriate backend
    void load_model(const std::string& model_name,
                    const std::string& checkpoint,
                    const std::string& recipe,
                    bool do_not_upgrade = true,
                    const std::vector<std::string>& labels = {});
    
    // Unload the currently loaded model
    void unload_model();
    
    // Get the currently loaded model info
    std::string get_loaded_model() const { return loaded_model_; }
    std::string get_loaded_checkpoint() const { return loaded_checkpoint_; }
    std::string get_loaded_recipe() const { return loaded_recipe_; }
    
    // Check if a model is loaded
    bool is_model_loaded() const { return wrapped_server_ != nullptr; }
    
    // Get backend server address (for streaming proxy)
    std::string get_backend_address() const;
    
    // Forward requests to the appropriate wrapped server
    json chat_completion(const json& request);
    json completion(const json& request);
    json embeddings(const json& request);
    json reranking(const json& request);
    json responses(const json& request);
    
    // Get telemetry data
    json get_stats() const;
    
private:
    std::unique_ptr<WrappedServer> wrapped_server_;
    std::string loaded_model_;
    std::string loaded_checkpoint_;
    std::string loaded_recipe_;
    bool unload_called_ = false;  // Track if unload has been called
    
    int ctx_size_;
    std::string llamacpp_backend_;
    std::string log_level_;
    
    // Concurrency control for load operations
    mutable std::mutex load_mutex_;              // Protects loading state and wrapped_server_
    bool is_loading_ = false;                    // True when a load operation is in progress
    std::condition_variable load_cv_;            // Signals when load completes
};

} // namespace lemon

