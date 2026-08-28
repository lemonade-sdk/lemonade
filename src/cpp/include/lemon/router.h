#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "wrapped_server.h"
#include "model_residency.h"
#include "model_manager.h"
#include "backend_manager.h"
#include "runtime_config.h"

// 5 seconds is generous enough for inference to complete but prevents
// indefinite blocking if a backend is stuck.
#define EVICTION_TIMEOUT 5

namespace lemon {

using json = nlohmann::json;

class CloudProviderRegistry;

// Single-thread executor that runs routing-helper reclaim tasks off the request
// / maintenance threads that trigger them. Owned by Router as a shared_ptr so a
// scheduled task holds only a weak reference and safely no-ops if the Router
// (and this executor) are torn down first. stop() joins the worker before the
// router destroys the state those tasks touch, so a task can never outlive it.
class RoutingHelperReclaimExecutor {
public:
    explicit RoutingHelperReclaimExecutor(std::function<void(const std::string&)> handler)
        : handler_(std::move(handler)), worker_([this] { run(); }) {}
    ~RoutingHelperReclaimExecutor() { stop(); }

    RoutingHelperReclaimExecutor(const RoutingHelperReclaimExecutor&) = delete;
    RoutingHelperReclaimExecutor& operator=(const RoutingHelperReclaimExecutor&) = delete;

    // Queue a helper for reclaim. Keyed by model name so repeated posts for the
    // same helper (marked -> dispatched -> rescued -> re-armed) collapse to one
    // pending entry instead of piling up redundant tasks.
    void post(const std::string& model_name) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            pending_.insert(model_name);
        }
        cv_.notify_one();
    }

    // Idempotent. Any still-queued names are dropped; the in-flight reclaim (if
    // any) is allowed to finish, then the worker is joined.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (stop_) return;
            std::string model_name = *pending_.begin();
            pending_.erase(pending_.begin());
            lock.unlock();
            try {
                handler_(model_name);
            } catch (...) {
            }
            lock.lock();
        }
    }

    std::function<void(const std::string&)> handler_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::set<std::string> pending_;
    bool stop_ = false;
    std::thread worker_;
};

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
    // Test-only access to routing-helper reconciliation internals (inject stub
    // servers, seed the needed set, drive prune). Defined in the test binary.
    friend struct RoutingHelperTestHook;
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
    void load_model(
        const std::string& model_name,
        const ModelInfo& model_info,
        RecipeOptions options,
        bool do_not_upgrade = true,
        bool allow_reload_on_option_change = false,
        std::optional<bool> pinned = std::nullopt,
        LoadPurpose load_purpose = LoadPurpose::UserInference,
        std::atomic<bool>* cancel_flag = nullptr);

    // Collapse the option precedence chain into the set a load would use.
    // *_args are resolved by scope: a concrete request replaces model/architecture
    // args, backend/machine args are inherited only while merge_args is enabled,
    // and overridable runtime defaults are materialized before this returns.
    // ctx_size may still be the -1 auto sentinel; the concrete value is only
    // resolved inside load_model, once eviction has freed memory.
    RecipeOptions resolve_effective_options(const ModelInfo& model_info,
                                            const RecipeOptions& request_options) const;

    // Apply request intent to an already-live process without reloading it.
    // Returns false when the requested model is not currently live.
    bool ensure_loaded_model_residency(
        const std::string& model_name,
        LoadPurpose load_purpose);

    // Record the authoritative set of routing-helper models the active policies
    // need resident (the union across all active policies, as policy-authored
    // names — resolved internally), then reclaim any live helper no longer in
    // it. The set is published immediately so a helper still mid-load validates
    // against it at load-completion (see load_model), making reconciliation
    // durable rather than a one-time snapshot; only the eviction pass is deferred
    // to a safe point. Pinned and busy helpers are left resident; a busy helper
    // is marked pending-stale and reclaimed the moment its last request releases
    // (see WrappedServer::release_inference), or by a later reconcile.
    // The generation orders concurrent policy notifications: an older generation
    // arriving after a newer one is ignored so it cannot republish a stale set.
    void reconcile_routing_helpers(const std::set<std::string>& needed_helper_models,
                                   uint64_t generation);

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
    // Pinned routing helpers, separated from the standard counts used by
    // the CLI's max_models preflight.
    json get_pinned_helper_counts() const;

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
    std::vector<std::string> audio_speech_supported_streaming_formats(const std::string& model_name);
    AudioFormatMetadata audio_speech_format_metadata(
        const std::string& model_name, const std::string& response_format);

    json image_generations(const json& request);
    json image_edits(const json& request);
    json image_variations(const json& request);

    void audio_generations(const json& request, httplib::DataSink& sink);
    std::vector<std::string> audio_generation_supported_formats(const std::string& model_name);
    AudioFormatMetadata audio_generation_format_metadata(
        const std::string& model_name, const std::string& response_format);
    void model_3d_generations(const json& request, httplib::DataSink& sink);

    void chat_completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void completion_stream(const std::string& request_body, httplib::DataSink& sink);
    void responses_stream(const std::string& request_body, httplib::DataSink& sink);

    json get_stats() const;

    // Get loaded backend metadata and per-model telemetry for metrics rendering.
    json get_metrics_snapshot() const;

    // Record one completed request's telemetry as a single atomic update.
    void update_request_telemetry(const std::string& model_name,
                                  const StreamingProxy::TelemetryData& telemetry);

    // Route-stability accounting for collection.router dispatch. The
    // fingerprint is a metrics key only (hash of the conversation's stable
    // prefix) — it never influences routing and stores no message content.
    void note_route_decision(uint64_t conversation_fingerprint, const std::string& route_to);

    bool begin_exclusive(std::atomic<bool>* cancel = nullptr);
    void end_exclusive();

    std::map<std::string, bool> snapshot_loaded_models() const;
    std::map<std::string, json> unload_job_models(const std::map<std::string, int>& owned_live,
                                                  const std::map<std::string, bool>& snapshot_pins);
    int loaded_model_pid(const std::string& model_name) const;
    std::string canonical_model_name(const std::string& model_name) const;

    // Registry lookup for seams that need ModelInfo without owning ModelManager
    // (e.g. routing CostServices). Returns nullopt when the name is unknown.
    std::optional<ModelInfo> try_get_model_info(const std::string& model_name) const;

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

    uint64_t routing_decisions_total_ = 0;
    uint64_t routing_switches_total_ = 0;
    std::list<uint64_t> route_fingerprint_lru_;
    std::unordered_map<uint64_t,
                       std::pair<std::string, std::list<uint64_t>::iterator>>
        route_last_target_;

    // Concurrency control for load operations
    mutable std::mutex load_mutex_;              // Protects loading state and loaded_servers_
    bool is_loading_ = false;                    // True when a load operation is in progress
    std::condition_variable load_cv_;            // Signals when load completes

    // Canonicalized routing-helper models the active policies need resident.
    // Guarded by load_mutex_; the authoritative snapshot a freshly loaded helper
    // validates itself against and a prune pass reclaims against.
    std::set<std::string> needed_helper_models_;
    // Highest policy-notification generation applied to needed_helper_models_.
    // Guarded by load_mutex_; an out-of-order (older) reconcile is ignored.
    uint64_t last_reconcile_generation_ = 0;
    // Set during ~Router (under load_mutex_) so a reclaim task waiting for the
    // residency slot to clear wakes and returns instead of blocking teardown.
    bool reclaim_shutdown_ = false;

    bool exclusive_active_ = false;
    std::thread::id exclusive_owner_;
    std::condition_variable exclusive_cv_;
    void wait_for_slot_clearance(std::unique_lock<std::mutex>& lock);

    std::unique_ptr<GlobalVramMonitor> vram_monitor_;
    std::unique_ptr<EvictionEngine> eviction_engine_;
    std::unique_ptr<SuspendInhibitor> suspend_inhibitor_;
    // Runs release-triggered routing-helper reclaims off the request/maintenance
    // threads. shared_ptr so a scheduled task holds a weak_ptr and no-ops if the
    // router is gone; stop()ped and joined in ~Router before state teardown.
    std::shared_ptr<RoutingHelperReclaimExecutor> reclaim_executor_;

    // Helper methods for multi-model management
    WrappedServer* find_server_by_model_name(const std::string& model_name) const;
    WrappedServer* get_most_recent_server() const;
    void prune_unavailable_servers_locked();
    bool reload_model_after_watchdog_reset(const std::string& requested_model, const RecipeOptions& options, uint64_t failed_instance_id = 0);
    bool is_watchdog_reset_response(const json& response) const;
    int count_servers_in_pool(ModelType type, ResidencyClass residency_class,
                              const std::string& model_name) const;
    int count_pinned_servers_in_pool(ModelType type,
                                      ResidencyClass residency_class) const;
    WrappedServer* find_lru_server_in_pool(ModelType type, ResidencyClass residency_class,
                                                  const std::string& model_name) const;
    void ensure_residency_capacity(ModelType type, ResidencyClass residency_class,
                                   const std::string& model_name);
    void transition_server_residency_locked(
        WrappedServer* server,
        ResidencyClass requested_residency_class);
    bool ensure_loaded_model_residency_canonical(
        const std::string& canonical_model_name,
        LoadPurpose load_purpose);
    bool has_npu_server() const;
    WrappedServer* find_npu_server() const;
    WrappedServer* find_npu_server_by_recipe(const std::string& recipe) const;
    WrappedServer* find_coexisting_server_by_type(ModelType type) const;
    void evict_all_npu_servers();
    void evict_server(WrappedServer* server, int timeout_seconds = -1);
    void evict_all_servers();
    // Publish the (already-canonicalized) needed-helper set under load_mutex_,
    // then defer the eviction pass until no load holds the slot. Split out from
    // reconcile_routing_helpers so the set is visible to a mid-load helper's
    // load-completion validation even while is_loading_ is still true.
    void apply_routing_helper_reconcile(std::set<std::string> needed, uint64_t generation);
    // Evict idle, unpinned routing helpers whose model is not in
    // needed_helper_models_. A busy not-needed helper is marked pending-stale so
    // it self-reclaims on its final release instead of blocking this pass on an
    // eviction timeout. Caller holds load_mutex_.
    void prune_stale_routing_helpers_locked();
    // Shared decision for a live routing helper (caller holds load_mutex_): if it
    // is still needed or pinned, cancel any pending reclaim and return false; if
    // it is stale but busy, arm the release-triggered reclaim and return false;
    // if it is stale and idle, return true so the caller evicts it now.
    bool reclaim_or_defer_helper_locked(WrappedServer* server);
    // Whether a routing-helper model is still referenced by an active policy
    // (present in needed_helper_models_). The single source of truth for the
    // needed-set membership test. Caller holds load_mutex_.
    bool is_needed_helper_locked(const std::string& canonical_model_name) const;
    // Whether a routing helper of the given residency/pin state is no longer
    // referenced by any active policy (absent from needed_helper_models_). Non
    // routing-helper and pinned models are never considered stale. Caller holds
    // load_mutex_.
    bool routing_helper_no_longer_needed(const std::string& canonical_model_name,
                                         ResidencyClass requested_residency_class,
                                         bool pinned) const;
    // Release-triggered reclaim: unload a pending-stale routing helper once its
    // last request drains. Looks the server up by name (never a captured raw
    // pointer) and re-validates staleness under load_mutex_. Waits for the
    // residency slot to clear (a load / exclusive session) rather than relying on
    // a later prune, so it is guaranteed to run and is safe to call from a thread
    // other than the one that ran release_inference / finish_downsize.
    void reclaim_stale_helper_if_idle(const std::string& model_name);
    // Install the busy->idle reclaim notifier on a server as it enters the router.
    // The notifier posts the server's model name to reclaim_executor_ via a
    // weak_ptr, so it never runs on the release call stack and no-ops if the
    // router is gone. Only fires when the server has been marked pending-stale.
    void install_reclaim_notifier(WrappedServer* server);
    // Eviction-engine entry point: physically unload a model previously marked
    // EVICTING, but only if it has not been rescued by an in-flight request
    // (see WrappedServer::try_commit_eviction). Safe against request races.
    void evict_if_committed(const std::string& model_name);
    std::unique_ptr<WrappedServer> create_backend_server(const ModelInfo& model_info);
    std::string resolve_model_name(const std::string& model_name) const;
    ModelTelemetryIdentity get_telemetry_identity(WrappedServer* server) const;
    void record_request_telemetry_for_model(const ModelTelemetryIdentity& identity,
                                            const StreamingProxy::TelemetryData& telemetry);

    template<typename Func>
    auto execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr));

    template<typename Func>
    void execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func, std::shared_ptr<telemetry::InferenceSpan> span = nullptr);
};

} // namespace lemon
