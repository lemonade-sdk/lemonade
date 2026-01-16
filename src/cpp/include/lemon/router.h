#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "wrapped_server.h"
#include "model_manager.h"

namespace lemon {

using json = nlohmann::json;

class Router {
public:
    Router(const json& default_options = json::object(),
           const std::string& log_level = "info",
           ModelManager* model_manager = nullptr,
           int max_llm_models = 1,
           int max_embedding_models = 1,
           int max_reranking_models = 1,
           int max_audio_models = 1,
           int max_image_models = 1);
    
    ~Router();
    
    // Load a model with the appropriate backend
    // Optional per-model settings override the defaults
    void load_model(const std::string& model_name,
                    const ModelInfo& model_info,
                    RecipeOptions options,
                    bool do_not_upgrade = true);
    
    // Unload model(s)
    void unload_model(const std::string& model_name = "");  // Empty = unload all
    
    // Get the most recently loaded model info (for backward compatibility)
    std::string get_loaded_model() const;
    std::string get_loaded_recipe() const;
    
    // Get all loaded models info
    json get_all_loaded_models() const;
    
    // Get max model limits
    json get_max_model_limits() const;
    
    // Check if any model is loaded
    bool is_model_loaded() const;
    
    // Check if a specific model is loaded
    bool is_model_loaded(const std::string& model_name) const;

    // Get the model type for a loaded model (returns LLM if not found)
    ModelType get_model_type(const std::string& model_name = "") const;
    
    // Get backend server address (for streaming proxy)
    std::string get_backend_address() const;
    
    // Forward requests to the appropriate wrapped server (non-streaming)
    json chat_completion(const json& request);
    json completion(const json& request);
    json embeddings(const json& request);
    json reranking(const json& request);
    json responses(const json& request);

    // Audio endpoints (OpenAI /v1/audio/* compatible)
    json audio_transcriptions(const json& request);

    // Image endpoints (OpenAI /v1/images/* compatible)
    json image_generations(const json& request);
    
    // Forward streaming requests to the appropriate wrapped server
    void chat_completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void responses_stream(const std::string& request_body, httplib::DataSink& sink);
    
    // Get telemetry data
    json get_stats() const;
    
    // Update telemetry data (for non-streaming requests)
    void update_telemetry(int input_tokens, int output_tokens, 
                         double time_to_first_token, double tokens_per_second);
    
    // Update prompt_tokens field from usage
    void update_prompt_tokens(int prompt_tokens);
    
private:
    // Multi-model support: Manage multiple WrappedServers
    std::vector<std::unique_ptr<WrappedServer>> loaded_servers_;
    
    // Configuration
    json default_options_;
    std::string log_level_;
    ModelManager* model_manager_;  // Non-owning pointer to ModelManager
    
    // Multi-model limits by type
    int max_llm_models_;
    int max_embedding_models_;
    int max_reranking_models_;
    int max_audio_models_;
    int max_image_models_;
    
    // Concurrency control for load operations
    mutable std::mutex load_mutex_;              // Protects loading state and loaded_servers_
    bool is_loading_ = false;                    // True when a load operation is in progress
    std::condition_variable load_cv_;            // Signals when load completes
    
    // Helper methods for multi-model management
    WrappedServer* find_server_by_model_name(const std::string& model_name) const;
    WrappedServer* get_most_recent_server() const;
    int count_servers_by_type(ModelType type) const;
    WrappedServer* find_lru_server_by_type(ModelType type) const;
    bool has_npu_server() const;
    WrappedServer* find_npu_server() const;
    void evict_server(WrappedServer* server);
    void evict_all_servers();
    std::unique_ptr<WrappedServer> create_backend_server(const ModelInfo& model_info);
    
    // Generic inference wrapper that handles locking and busy state
    template<typename Func>
    auto execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr));
    
    // Generic streaming wrapper
    template<typename Func>
    void execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func);
};

} // namespace lemon

