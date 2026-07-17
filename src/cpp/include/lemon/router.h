#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <condition_variable>
#include <thread>
#include <vector>
#include <optional>
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

class CloudProviderRegistry;

namespace telemetry {
class InferenceSpan;
}

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

class EvictionEngine;
class GlobalVramMonitor;
class SuspendInhibitor;

class Router {
public:
    friend class EvictionEngine;
    Router(RuntimeConfig* config,

           ModelManager* model_manager,
           BackendManager* backend_manager);

    ~Router();

    // Wires the cloud provider registry so the Router can construct
    // CloudServer instances with a credential source. Pointer (not
    // ownership) — Server owns the registry.
    void set_cloud_registry(CloudProviderRegistry* registry);

    // allow_reload_on_option_change: intended for explicit /load callers only.
    // Auto-load callers (inference-triggered) should leave this false so they
    // don't overturn options set by a prior explicit /load.
    void load_model(const std::string& model_name,
                    const ModelInfo& model_info,
                    RecipeOptions options,
                    bool do_not_upgrade = true,
                    bool allow_reload_on_option_change = false,
                    std::optional<bool> pinned = std::nullopt,
                    std::atomic<bool>* cancel_flag = nullptr);

    void unload_model(const std::string& model_name = "");  // Empty = unload all

    std::string get_loaded_model() const;
    std::string get_loaded_recipe() const;

    // The single live model of this type, or "" when none or more than one is
    // loaded. Endpoints that let the caller omit "model" use this so an
    // ambiguous choice is refused rather than silently resolved.
    std::string get_sole_loaded_model_of_type(ModelType type) const;

    json get_all_loaded_models() const;

    json get_max_model_limits() const;

    // Get pinned model counts per type
    json get_pinned_model_counts() const;

    // Pin or unpin a model
    void set_model_pinned(const std::string& model_name, bool pinned);

    bool is_model_loaded() const;

    bool is_model_loaded(const std::string& model_name) const;

    RecipeOptions get_model_recipe_options(const std::string& model_name) const;

    ModelType get_model_type(const std::string& model_name = "") const;

    std::string get_backend_address() const;

    // Get the streaming transcription address for the given model (falls back
    // to the most recently used server when model_name is empty).
    // Returns empty string if the backend does not support streaming transcription.
    std::string get_streaming_transcription_address(const std::string& model_name) const;

    json chat_completion(const json& request, std::atomic<bool>* cancel = nullptr);
    json completion(const json& request);
    json embeddings(const json& request);
    json reranking(const json& request);
    json classify(const json& request);
    json get_slots();
    json slots_action(int slot_id, const std::string& action, const json& request_body);
    json tokenize(const json& request);
    json responses(const json& request);

    json audio_transcriptions(const json& request);
    void audio_speech(const json& request, httplib::DataSink& sink);
    std::vector<std::string> audio_speech_supported_formats(const std::string& model_name);

    json image_generations(const json& request);
    json image_edits(const json& request);
    json image_variations(const json& request);

    void audio_generations(const json& request, httplib::DataSink& sink);
    std::vector<std::string> audio_generation_supported_formats(const std::string& model_name);
    void model_3d_generations(const json& request, httplib::DataSink& sink);

    void chat_completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void responses_stream(const std::string& request_body, httplib::DataSink& sink);

    json get_stats() const;

    // Get loaded backend metadata and per-model telemetry for metrics rendering.
    json get_metrics_snapshot() const;

    void update_telemetry(const std::string& model_name,
                         int input_tokens, int output_tokens,
                         double time_to_first_token, double tokens_per_second);

    void update_prompt_tokens(const std::string& model_name, int prompt_tokens);

    bool begin_exclusive(std::atomic<bool>* cancel = nullptr);
    void end_exclusive();

    std::map<std::string, bool> snapshot_loaded_models() const;
    std::map<std::string, json> unload_job_models(const std::map<std::string, int>& owned_live,
                                                  const std::map<std::string, bool>& snapshot_pins);
    int loaded_model_pid(const std::string& model_name) const;
    std::string canonical_model_name(const std::string& model_name) const;

    // Test hooks
    void simulate_vram_pressure(double pct);

private:
    // Multi-model support: Manage multiple WrappedServers
    std::vector<std::unique_ptr<WrappedServer>> loaded_servers_;

    // Configuration (non-owning pointer; same lifetime as Server)
    RuntimeConfig* config_;
    ModelManager* model_manager_;  // Non-owning pointer to ModelManager
    BackendManager* backend_manager_;  // Non-owning pointer to BackendManager
    CloudProviderRegistry* cloud_registry_ = nullptr;  // Non-owning

    mutable std::mutex telemetry_mutex_;
    Telemetry aggregate_telemetry_;
    std::map<std::string, ModelTelemetryRecord> telemetry_by_model_;

    // Concurrency control for load operations
    mutable std::mutex load_mutex_;              // Protects loading state and loaded_servers_
    bool is_loading_ = false;                    // True when a load operation is in progress
    std::condition_variable load_cv_;            // Signals when load completes

    bool exclusive_active_ = false;
    std::thread::id exclusive_owner_;
    std::condition_variable exclusive_cv_;
    void wait_for_slot_clearance(std::unique_lock<std::mutex>& lock);

    std::unique_ptr<GlobalVramMonitor> vram_monitor_;
    std::unique_ptr<EvictionEngine> eviction_engine_;
    std::unique_ptr<SuspendInhibitor> suspend_inhibitor_;

    // Helper methods for multi-model management
    WrappedServer* find_server_by_model_name(const std::string& model_name) const;
    WrappedServer* get_most_recent_server() const;
    void prune_unavailable_servers_locked();
    bool reload_model_after_watchdog_reset(const std::string& requested_model, const RecipeOptions& options);
    bool is_watchdog_reset_response(const json& response) const;
    int count_servers_by_type(ModelType type) const;
    int count_pinned_servers_by_type(ModelType type) const;
    WrappedServer* find_lru_server_by_type(ModelType type) const;
    bool has_npu_server() const;
    WrappedServer* find_npu_server() const;
    WrappedServer* find_npu_server_by_recipe(const std::string& recipe) const;
    WrappedServer* find_coexisting_server_by_type(ModelType type) const;
    void evict_all_npu_servers();
    void evict_server(WrappedServer* server, int timeout_seconds = -1);
    void evict_all_servers();
    // Eviction-engine entry point: physically unload a model previously marked
    // EVICTING, but only if it has not been rescued by an in-flight request
    // (see WrappedServer::try_commit_eviction). Safe against request races.
    void evict_if_committed(const std::string& model_name);
    std::unique_ptr<WrappedServer> create_backend_server(const ModelInfo& model_info);
    std::string resolve_model_name(const std::string& model_name) const;
    ModelTelemetryIdentity get_telemetry_identity(WrappedServer* server) const;
    void record_telemetry_for_model(const ModelTelemetryIdentity& identity,
                                    int input_tokens,
                                    int output_tokens,
                                    double time_to_first_token,
                                    double tokens_per_second);
    void record_prompt_tokens_for_model(const ModelTelemetryIdentity& identity, int prompt_tokens);

    template<typename Func>
    auto execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr));

    template<typename Func>
    void execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func, std::shared_ptr<telemetry::InferenceSpan> span = nullptr);
};

} // namespace lemon
