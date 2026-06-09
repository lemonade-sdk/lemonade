#pragma once

#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "wrapped_server.h"
#include "model_manager.h"
#include "backend_manager.h"
#include "runtime_config.h"

// 5 seconds is generous enough for inference to complete but prevents
// indefinite blocking if a backend is stuck.
#define EVICTION_TIMEOUT 5

namespace lemon {

using json = nlohmann::json;

struct ModelTelemetryIdentity {
    std::string model_name;
    std::string checkpoint;
    std::string type;
    std::string device;
    std::string recipe;

    std::string key() const {
        return model_name + "\n" + checkpoint + "\n" + type + "\n" + device + "\n" + recipe;
    }
};

struct ModelTelemetryRecord {
    ModelTelemetryIdentity identity;
    Telemetry telemetry;
};

class Router {
public:
    Router(RuntimeConfig* config,
           ModelManager* model_manager,
           BackendManager* backend_manager);

    ~Router();

    // Load a model with the appropriate backend
    // Optional per-model settings override the defaults
    // allow_reload_on_option_change: intended for explicit /load callers only.
    // Auto-load callers (inference-triggered) should leave this false so they
    // don't overturn options set by a prior explicit /load.
    void load_model(const std::string& model_name,
                    const ModelInfo& model_info,
                    RecipeOptions options,
                    bool do_not_upgrade = true,
                    bool allow_reload_on_option_change = false);

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

    // Get the recipe options for a loaded model (empty if not loaded)
    RecipeOptions get_model_recipe_options(const std::string& model_name) const;

    // Get the model type for a loaded model (returns LLM if not found)
    ModelType get_model_type(const std::string& model_name = "") const;

    // Get backend server address (for streaming proxy)
    std::string get_backend_address() const;

    // Forward requests to the appropriate wrapped server (non-streaming)
    json chat_completion(const json& request);
    json completion(const json& request);
    json embeddings(const json& request);
    json reranking(const json& request);
    json get_slots();
    json slots_action(int slot_id, const std::string& action, const json& request_body);
    json tokenize(const json& request);
    json responses(const json& request);

    // Audio endpoints (OpenAI /v1/audio/* compatible)
    json audio_transcriptions(const json& request);
    void audio_speech(const json& request, httplib::DataSink& sink);

    // Image endpoints (OpenAI /v1/images/* compatible)
    json image_generations(const json& request);
    json image_edits(const json& request);
    json image_variations(const json& request);

    // Forward streaming requests to the appropriate wrapped server
    void chat_completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void responses_stream(const std::string& request_body, httplib::DataSink& sink);

    // Get telemetry data
    json get_stats() const;

    // Get loaded backend metadata and per-model telemetry for metrics rendering.
    json get_metrics_snapshot() const;

    // Update telemetry data (for non-streaming requests)
    void update_telemetry(const std::string& model_name,
                         int input_tokens, int output_tokens,
                         double time_to_first_token, double tokens_per_second);

    // Update prompt_tokens field from usage
    void update_prompt_tokens(const std::string& model_name, int prompt_tokens);

private:
    // Multi-model support: Manage multiple WrappedServers
    std::vector<std::unique_ptr<WrappedServer>> loaded_servers_;

    // Configuration (non-owning pointer; same lifetime as Server)
    RuntimeConfig* config_;
    ModelManager* model_manager_;  // Non-owning pointer to ModelManager
    BackendManager* backend_manager_;  // Non-owning pointer to BackendManager
    mutable std::mutex telemetry_mutex_;
    Telemetry aggregate_telemetry_;
    std::map<std::string, ModelTelemetryRecord> telemetry_by_model_;

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
    WrappedServer* find_npu_server_by_recipe(const std::string& recipe) const;
    WrappedServer* find_flm_server_by_type(ModelType type) const;
    void evict_all_npu_servers();
    void evict_server(WrappedServer* server);
    void evict_all_servers();
    std::unique_ptr<WrappedServer> create_backend_server(const ModelInfo& model_info);
    std::string resolve_model_name(const std::string& model_name) const;
    ModelTelemetryIdentity get_telemetry_identity(WrappedServer* server) const;
    void record_telemetry_for_model(const ModelTelemetryIdentity& identity,
                                    int input_tokens,
                                    int output_tokens,
                                    double time_to_first_token,
                                    double tokens_per_second);
    void record_prompt_tokens_for_model(const ModelTelemetryIdentity& identity, int prompt_tokens);

    // Generic inference wrapper that handles locking and busy state
    template<typename Func>
    auto execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr));

    // Generic streaming wrapper
    template<typename Func>
    void execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func);
};

} // namespace lemon
