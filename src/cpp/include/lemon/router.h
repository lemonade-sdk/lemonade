#pragma once

#include <string>
#include <memory>
#include <shared_mutex>
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
    
    // Get the currently loaded model info (thread-safe)
    std::string get_loaded_model() const;
    std::string get_loaded_checkpoint() const;
    std::string get_loaded_recipe() const;
    
    // Check if a model is loaded (thread-safe)
    bool is_model_loaded() const;
    
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
    
    mutable std::shared_mutex load_mutex_;  // Reader-writer lock: readers can read concurrently, writers are exclusive
    
    // Internal helper that does the actual unload work (assumes write lock is already held)
    void unload_model_impl();
};

} // namespace lemon

