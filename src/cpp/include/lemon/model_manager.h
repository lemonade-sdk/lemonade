#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "canonical_id.h"
#include "directory_watcher.h"
#include "gguf_reader.h"
#include "model_types.h"
#include "recipe_options.h"

namespace lemon {

using json = nlohmann::json;

// Thrown by ModelManager::download_model when a pull request names a model
// that (a) is not registered, (b) is not in the filtered-out registry, and
// (c) lacks the `user.` prefix that would make it a new-model registration
// attempt.
//
// CONTRACT: the /pull HTTP handler catches this type and attaches
// {"code": kUnknownModelErrorCode, ...} to the error response. The lemonade
// CLI keys off that code to replace the message with a friendlier one that
// points at `lemonade list` and `lemonade pull CHECKPOINT`. The CLI inlines
// the "unknown_model" literal to avoid pulling this server header into the
// CLI; update cli/lemonade_client.cpp in lockstep if this constant changes.
class UnknownModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
constexpr const char* kUnknownModelErrorCode = "unknown_model";

// Thrown when a definition names a deployment mode its recipe's backend cannot
// serve. Registering it would list the model as something it is not and then
// fail at inference time. A definition naming *no* mode is not an error: the
// recipe's default is stamped instead.
//
// CONTRACT: the /pull HTTP handler catches this type and returns 400. Loading an
// already-persisted entry does not go through registration, so an entry written
// by an older version is still normalized rather than blocking startup.
class InvalidModelDefinitionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Progress information for download operations
struct DownloadProgress {
    std::string file;           // Current file being downloaded
    int file_index = 0;         // Current file index (1-based)
    int total_files = 0;        // Total number of files to download
    size_t bytes_downloaded = 0; // Bytes downloaded for current file
    size_t bytes_total = 0;     // Total bytes for current file
    size_t total_download_size = 0; // Total bytes across ALL files in this download
    size_t bytes_previously_downloaded = 0; // Bytes already on disk (resume offset or skipped file)
    int percent = 0;            // Overall percentage (0-100)
    bool complete = false;      // True when all downloads finished
    std::string error;          // Error message if failed
};

// Callback for download progress updates
// Returns bool: true = continue download, false = cancel download
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;

// Parsed collection.router routing policy (defined in routing_policy.h). Only
// forward-declared here so this widely-included header stays light; ModelInfo
// holds it behind a shared_ptr, which supports incomplete types.
struct RoutePolicy;

// Image generation defaults for SD models
struct ImageDefaults {
    int steps = 20;
    float cfg_scale = 7.0f;
    int width = 512;
    int height = 512;
    std::string sampling_method;
    float flow_shift = 0.0f;

    bool has_defaults = false;  // True if explicit defaults were provided in JSON
};

struct ModelInfo {
    std::string model_name;
    std::map<std::string, std::string> checkpoints;
    std::map<std::string, std::string> resolved_paths; // Absolute path to model file/directory on disk
    std::string recipe;
    std::vector<std::string> labels;
    std::vector<std::string> components;
    std::vector<std::string> input_aliases;  // Names accepted in requests but hidden from /models
    bool suggested = false;
    std::string source;  // Local origin: local_upload/local_path/extra_models_dir
    std::string registry_source = "huggingface";  // Remote registry: huggingface/modelscope
    bool downloaded = false;     // Whether model is downloaded and available
    bool update_available = false; // Whether a newer remote-registry version exists
    std::optional<bool> auto_update = std::nullopt; // Optional per-model auto-update override
    double size = 0.0;   // Model size in GB
    // Resident working set for a streaming backend; 0 = filter on full `size`.
    // See streaming_working_set_gb() / filter_models_by_backend.
    double min_resident_gb = 0.0;
    int64_t max_context_window = 0;  // Static model-supported text context, when known

    // GGUF architecture metadata (populated for llamacpp models, used for auto ctx_size)
    GgufMetadata gguf;
    RecipeOptions recipe_options;

    // Multi-model support fields
    ModelType type = ModelType::LLM;      // Model type for LRU cache management
    DeviceType device = DEVICE_NONE;      // Target device(s) for this model

    // Image generation defaults (for sd-cpp models)
    ImageDefaults image_defaults;

    // Per-collection system prompt template (collection.omni models only).
    // When non-empty, overrides the global default in toolDefinitions.json.
    // Stays a template — {tool_list} / {tool_guidance} are substituted at runtime.
    std::string system_prompt;

    // Cloud offload (for "cloud" recipe). Names the provider to dispatch to
    // (e.g., "fireworks"). Empty for non-cloud recipes.
    std::string cloud_provider;
    // Per-token price in USD per 1,000,000 tokens, when the provider reports it
    // (OpenRouter, Together). <0 means unknown (e.g. Fireworks doesn't publish
    // pricing in /v1/models). Surfaced on /v1/models for display, and (when a
    // collection.router policy is evaluated) attached to the decision as
    // illustrative outputs.estimated_cost — not a billing figure.
    double cost_input_per_million = -1.0;
    double cost_output_per_million = -1.0;

    // Generic per-model fields a backend declares for itself. Any server_models.json
    // key not consumed by a typed field above lands here, so a new backend can read
    // custom per-model config in load() without editing this shared struct.
    std::map<std::string, json> extras;

    // Parsed routing policy for collection.router models. Populated once when the
    // models cache is built (from recipe + components + the "routing" block in
    // extras) so request-time dispatch reads it directly instead of re-parsing.
    // Null for every other recipe. shared_ptr keeps ModelInfo copies cheap.
    std::shared_ptr<const RoutePolicy> route_policy;

    // Look up an extra field, returning a default when absent.
    template <typename T>
    T extra(const std::string& key, const T& fallback) const {
        auto it = extras.find(key);
        if (it == extras.end() || it->second.is_null()) return fallback;
        try { return it->second.get<T>(); } catch (...) { return fallback; }
    }

    // Utility
    std::string checkpoint(const std::string& type = "main") const { return checkpoints.count(type) ? checkpoints.at(type) : ""; }
    std::string resolved_path(const std::string& type = "main") const { return resolved_paths.count(type) ? resolved_paths.at(type) : ""; }

    std::string mmproj() const { return checkpoint("mmproj"); }
};

struct ModelFileInfo {
    std::string name;
    std::string path;
    std::string role;
    std::uint64_t size_bytes = 0;
    bool exists = false;
};

class CloudProviderRegistry;

class ModelManager {
public:
    explicit ModelManager(const std::string& extra_models_dir = "");

    // Joins the watcher thread. Required, not incidental: the watcher callback
    // locks models_cache_mutex_, which is declared after directory_watcher_ and
    // so freed first by reverse-order destruction.
    ~ModelManager();

    std::map<std::string, ModelInfo> discover_extra_models_for_test() const {
        return discover_extra_models();
    }

    // Wires the cloud provider registry. ModelManager uses it to look up
    // {base_url, api_key} per provider when refreshing cloud models during
    // build_cache(). Pointer (not ownership) — Server owns the registry.
    // Must be called before the first build_cache() / get_supported_models().
    void set_cloud_registry(CloudProviderRegistry* registry);

    // The wired registry, or nullptr if set_cloud_registry was never called.
    CloudProviderRegistry* cloud_registry() const { return cloud_registry_; }

    // Refresh discovered models for one provider. Looks up creds via the
    // registry, calls CloudServer::discover_models, and re-seeds the
    // provider's entries (drop-then-add semantics). No-op + warning if the
    // provider has no resolvable key. Returns the number of models present
    // after refresh. Throws never — errors logged, empty result returned.
    size_t refresh_cloud_models(const std::string& provider);

    // Drop every cached model for one provider (used by uninstall). Returns
    // the count removed. Doesn't touch the registry — caller already did.
    size_t evict_cloud_models(const std::string& provider);

    // Count of currently-cached cloud models for a provider. For system-info.
    size_t count_cloud_models(const std::string& provider) const;

    // Invalidate the models cache (e.g. after backend install/uninstall)
    void invalidate_models_cache();

    // Register a callback fired after a router collection's policy changes (it
    // is added, edited, or removed — via the API or an on-disk edit picked up by
    // the directory watcher). Used to reconcile router-collection policy state,
    // e.g. evicting routing helpers no active policy still references. Invoked
    // outside all ModelManager locks. Set once during startup. The callback
    // receives a monotonic generation number identifying this change; consumers
    // use it to discard notifications that arrive out of order.
    void set_models_changed_callback(std::function<void(uint64_t)> cb);

    // Reserve the next monotonic registry-change generation. Callers that
    // reconcile registry state outside notify_models_changed() (e.g. the startup
    // seed) use this so their publication participates in the same ordering.
    uint64_t next_notify_generation();

    // Get all supported models from server_models.json
    std::map<std::string, ModelInfo> get_supported_models();

    // Get downloaded models
    std::map<std::string, ModelInfo> get_downloaded_models();

    // Filter models by available backends. Set track_recipe_availability only on
    // the full-cache build: the single-model temp maps used by incremental
    // updates must not overwrite the whole-registry availability side table.
    std::map<std::string, ModelInfo> filter_models_by_backend(
        const std::map<std::string, ModelInfo>& models,
        bool track_recipe_availability = false);

    // Register a user model
    void register_user_model(const std::string& model_name,
                            const json& model_data,
                            const std::string& source = "");

    // Register or validate a model definition without downloading its files.
    // Uses the same registration path as download_model.
    void register_model(const std::string& model_name,
                       const json& model_data,
                       bool allow_missing_checkpoint = false,
                       bool replace_existing = false);

    // Register (if needed) and download a model
    void download_model(const std::string& model_name,
                       const json& model_data,
                       bool do_not_upgrade = false,
                       DownloadProgressCallback progress_callback = nullptr);

    // Download a model
    void download_registered_model(const ModelInfo& info,
                                bool do_not_upgrade = false,
                                DownloadProgressCallback progress_callback = nullptr);

    // Delete a model
    void delete_model(const std::string& model_name);

    // Clean up orphaned files from multi-repo models downloaded in old layout
    nlohmann::json cleanup_orphaned_cache(bool dry_run);

    // Get model info by name
    ModelInfo get_model_info(const std::string& model_name);

    // Get per-model file inventory for the Files tab.
    std::vector<ModelFileInfo> list_model_files(const std::string& model_name);

    // Resolve a public model reference to its canonical internal name.
    std::string resolve_model_name(const std::string& model_name);

    // Get the public name exposed by Lemonade APIs for a canonical model name.
    std::string get_public_model_name(const std::string& model_name);

    // Check if model exists (in filtered list based on system capabilities)
    bool model_exists(const std::string& model_name);

    // Validate a collection (recipe="collection.omni") registration request.
    // Returns nullopt on success, or a user-facing error message on failure.
    // Used by /pull request validation and as a defensive guard in download_model.
    std::optional<std::string> validate_collection_request(
        const std::string& model_name, const nlohmann::json& model_data);

    // Check if model exists in the raw registry (before filtering)
    // Returns true even for NPU models on systems without NPU
    bool model_exists_unfiltered(const std::string& model_name);

    // Get model info from raw registry (without filtering)
    // Useful for generating helpful error messages about unsupported models
    ModelInfo get_model_info_unfiltered(const std::string& model_name);

    // Get the reason why a model was filtered out (empty string if not filtered)
    // Returns a user-friendly message explaining why the model is not available
    std::string get_model_filter_reason(const std::string& model_name);

    // Real-backend recipes with nothing runnable on this system because every
    // one of their built-in models was filtered out by the system-memory
    // heuristic (and no user model fills the gap). Callers use this to hide such
    // backends from the recipe/backends listing. Recipes filtered for
    // hardware/OS reasons are excluded (they keep their "unsupported" display).
    // Builds the cache if needed.
    std::set<std::string> recipes_with_all_models_filtered();

    // The set difference behind the above: recipes that had a model dropped by
    // the memory heuristic and kept none visible. Pure and hardware-independent
    // so it can be unit-tested directly.
    static std::set<std::string> recipes_missing_all_models(
        const std::set<std::string>& size_filtered_recipes,
        const std::set<std::string>& visible_recipes);

    // Memory-fit helpers for streaming backends (see filter_models_by_backend
    // for the rationale). Pure and hardware-independent, so unit-tested directly.
    static double streaming_working_set_gb(double min_resident_gb, double size_gb);
    static bool streaming_model_exceeds_pool(double working_set_gb, double pool_gb);

    // Test-only raw view of the side table without a cache rebuild; prefer
    // recipes_with_all_models_filtered() everywhere else.
    std::set<std::string> recipes_all_models_filtered_snapshot() const;

    // Check if model is downloaded
    bool is_model_downloaded(const std::string& model_name);

struct UpdateCheckResult {
    std::vector<std::string> updated_models;
    std::vector<std::string> up_to_date_models;
    std::map<std::string, std::string> failed_models;
};

    // Check downloaded models for updates in their configured remote registry.
    // Fetches the latest commit SHA for each model's repo and compares it
    // with the cached commit. Sets update_available on models whose upstream
    // repo has changed and clears stale flags for repos that were successfully
    // verified as current. If targets is non-empty, restricts check to specified targets.
    // Safe to call from a background thread — locks are internal.
    UpdateCheckResult check_for_model_updates(const std::vector<std::string>& targets = {});


    // Check if model should be automatically updated when updates are detected
    bool should_auto_update(const ModelInfo& info) const;

    // Register a callback triggered when a model's files are updated on disk (e.g. to evict loaded router backends).
    void set_model_updated_callback(std::function<void(const std::string&)> cb) {
        on_model_updated_cb_ = std::move(cb);
    }

    // Register a callback triggered during synchronization phases (e.g. for deterministic unit testing).
    void set_sync_phase_callback(std::function<void(const std::string&)> cb) {
        sync_phase_callback_ = std::move(cb);
    }

    // Set update_available in cache for deterministic unit testing.
    void set_model_update_available_for_test(const std::string& model_name, bool available) {
        std::string canonical = resolve_model_name(model_name);
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto it = models_cache_.find(canonical);
        if (it != models_cache_.end()) {
            it->second.update_available = available;
        }
    }

    // Override update check result for deterministic unit testing.
    void set_update_check_override_for_test(std::function<UpdateCheckResult(const std::vector<std::string>&)> override_fn) {
        update_check_override_ = std::move(override_fn);
    }

    // Override download behavior for deterministic unit testing.
    void set_download_model_override_for_test(std::function<void(const std::string&, const json&, bool, DownloadProgressCallback)> override_fn) {
        download_model_override_ = std::move(override_fn);
    }

    // Cancel active model synchronization.
    void cancel_sync();


    struct SyncEnqueueResult {
        bool already_running = false;
        uint64_t sync_id = 0;
    };

    // Query model sync status (running state, active/pending targets, completed count, or specific sync_id outcome)
    json get_sync_status(uint64_t sync_id = 0) const;

    // Synchronously queue targets for sync. Returns dispatch result with sync_id and whether sync was already in progress.
    SyncEnqueueResult enqueue_sync(const std::vector<std::string>& target_models = {}, bool attach_if_running = false);

    // Execute background queue processing until empty.
    json execute_sync();

    // Trigger sync/update of specified or all outdated models.
    // When target_models is empty, targets all downloaded outdated models.
    // If dry_run is true, returns update status without downloading files.
    json sync_models(const std::vector<std::string>& target_models = {}, bool dry_run = false, bool attach_if_running = false);


    // True if the model's backend pulls its own models on demand (e.g. flm) and
    // so should be skipped by the router's load-time auto-download path.
    bool backend_self_manages_downloads(const std::string& recipe) const;

    // Shared registry-backed completeness check: true if all required checkpoints
    // are present and complete (per-backend file validation runs via ops).
    bool checkpoints_complete(const ModelInfo& info) const;

    // Shared remote-registry download engine. The default BackendOps::download_model
    // delegates here; flm/cloud override with their own download.
    void download_from_registry_engine(const ModelInfo& info,
                                       DownloadProgressCallback progress_callback = nullptr);

    // Source-compatible alias for integrations built against the original API.
    // The model's registry_source still controls which provider is contacted.
    void download_from_huggingface_engine(const ModelInfo& info,
                                          DownloadProgressCallback progress_callback = nullptr);

    // Get shared model-hub cache directory (respects HF_HUB_CACHE, HF_HOME, and platform defaults)
    std::string get_hf_cache_dir() const;

    // Set extra models directory for GGUF discovery.
    // Starts/stops an inotify (Linux) / kqueue (macOS) watcher that
    // automatically refreshes the model cache when files are added or
    // removed in the directory.
    void set_extra_models_dir(const std::string& dir);

    // Per-architecture default recipe options (loaded from resources).
    // Override global config defaults but are overridden by model-level recipe_options.
    json get_architecture_defaults(const std::string& architecture) const;

    void save_model_options(const ModelInfo& info);

    // The model's own entry in recipe_options.json, i.e. only what the user
    // explicitly saved. Empty object when the model has no entry.
    json get_saved_model_options(const std::string& model_name);

    // What the model would resolve to with its recipe_options.json entry
    // removed: image_defaults plus the registry JSON's own recipe_options.
    RecipeOptions get_model_default_options(const ModelInfo& info);

    // Replace the model's recipe_options.json entry, returning the new entry.
    // An empty object erases the entry rather than persisting `{}`.
    json set_saved_model_options(const std::string& model_name, const json& saved);

    // Merge changes into the model's recipe_options.json entry and return the
    // new entry. A null value erases that key. Atomic with respect to other
    // writers of the same entry.
    json update_saved_model_options(const std::string& model_name, const json& changes);

    // The model-level options update_saved_model_options(changes) would leave
    // the model with, computed without persisting anything.
    RecipeOptions preview_saved_model_options(const ModelInfo& info, const json& changes);

    void start_directory_watcher();

private:
    // Cycle-detecting overload used by the collection fan-out in download_model.
    // `visited` accumulates collection names already entered on the current
    // call chain; re-entering one throws.
    void download_model(const std::string& model_name,
                       const json& model_data,
                       bool do_not_upgrade,
                       DownloadProgressCallback progress_callback,
                       std::set<std::string>& visited,
                       bool register_only,
                       bool allow_missing_checkpoint,
                       bool replace_existing);

    json load_server_models();
    json load_architecture_defaults();
    json load_optional_json(const std::string& path);
    void save_user_models(const json& user_models);

    // Remove a user model entry from user_models.json (no file deletion).
    // Used to roll back a collection registered earlier in the same call when
    // its component resolution fails.
    void unregister_user_model(const std::string& model_name);

    std::string get_user_models_file();
    std::string get_recipe_options_file();

    // Collection manifests (recipe="collection.omni" with a registry checkpoint):
    // the full collection definition lives in the configured remote registry as
    // an exported collection JSON (discovered by content, not filename).
    nlohmann::json fetch_collection_manifest(const std::string& repo_id,
                                               const std::string& registry_source,
                                               bool do_not_upgrade);

    // Resolve a collection's component list against the registry: known names
    // keep the local definition (local-wins, drift logged); unknown names are
    // registered as `user.` models from their inline definition in
    // `component_defs` (the `models` array of a collection file/manifest).
    // Returns the components as canonical cache names, preserving order.
    std::vector<std::string> register_components(const nlohmann::json& component_names,
                                                 const nlohmann::json& component_defs,
                                                 const std::string& registry_source = "huggingface");

    // Resolve a registry-backed collection's components at pull time: fetch the
    // manifest, then register_components() against its components/models arrays.
    std::vector<std::string> resolve_collection_components_from_manifest(
        const std::string& repo_id,
        const std::string& registry_source, bool do_not_upgrade);

    // Populate a collection's components from a manifest already cached on disk
    // (offline, no registration). Used by build_cache so a pulled collection keeps
    // its components across restarts. No-op if the manifest is not cached.
    // Caller must hold models_cache_mutex_ (reads server_models_/user_models_).
    void populate_collection_components_from_cache_locked(ModelInfo& info);

    // Fire the models-changed callback (if set) outside all locks. Guarded
    // against same-thread reentrancy so a callback that reads the registry
    // cannot recursively re-fire.
    void notify_models_changed();

    json registry_recipe_options(const std::string& cache_key);
    // Caller must hold models_cache_mutex_.
    json registry_recipe_options_locked(const std::string& cache_key);
    json write_saved_model_options(const std::string& model_name, const json& options, bool merge);

    // Cache management
    void build_cache();
    void add_model_to_cache(const std::string& model_name);
    // Caller must hold models_cache_mutex_.
    void update_model_options_in_cache_locked(const ModelInfo& info);
    void update_model_in_cache(const std::string& model_name, bool downloaded);
    void remove_model_from_cache(const std::string& model_name);

    // Resolve model checkpoint to absolute path on disk
    std::string resolve_model_path(const ModelInfo& info, const std::string& type, const std::string& checkpoint) const;
    void resolve_all_model_paths(ModelInfo& info);

    // Download from a JSON manifest
    void download_from_manifest(const json& manifest, std::map<std::string, std::string>& headers, DownloadProgressCallback progress_callback);

    // Download from the model's configured remote registry
    void download_from_registry(const ModelInfo& info,
                                   DownloadProgressCallback progress_callback = nullptr);

    // Discover GGUF models from extra_models_dir
    std::map<std::string, ModelInfo> discover_extra_models() const;

    ModelInfo init_extra_model_info(const std::string& name) const;

    // Scan one extra_models_dir subfolder and add either one folder model or one model per variant.
    void discover_extra_models_in_directory(
        const std::filesystem::path& dir_path,
        const std::vector<std::filesystem::path>& gguf_files,
        std::map<std::string, ModelInfo>& discovered,
        const std::filesystem::path& search_path) const;

    json server_models_;
    json user_models_;
    json recipe_options_;
    json architecture_defaults_;  // Per-architecture recipe option overlays (from resources)
    std::string extra_models_dir_;  // Secondary directory for GGUF model discovery
    CloudProviderRegistry* cloud_registry_ = nullptr;  // Not owned
    std::unique_ptr<DirectoryWatcher> directory_watcher_;

    // Fired after the model registry changes (add/edit/remove). Guarded by its
    // own mutex; invoked outside all other ModelManager locks.
    std::mutex models_changed_callback_mutex_;
    std::function<void(uint64_t)> models_changed_callback_;
    // Monotonic counter identifying each registry change. The callback may read
    // the live registry to compute a snapshot and publish it downstream;
    // concurrent runs can publish an older snapshot after a newer one, so the
    // generation lets the consumer keep only the newest.
    std::atomic<uint64_t> notify_generation_{0};

    // Cache of all models with their download status
    mutable std::mutex models_cache_mutex_;
    // Orders recipe_options.json rewrites without holding models_cache_mutex_
    // (which every request thread contends on) across disk I/O.
    std::mutex recipe_options_write_mutex_;

    // Serializes concurrent downloads that write into the same snapshot
    // (keyed by checkpoint repo). See download_registered_model.
    std::mutex download_locks_mutex_;
    std::map<std::string, std::shared_ptr<std::mutex>> download_locks_;

    // Prevent startup and manual update checks from running concurrently.
    std::mutex update_check_mutex_;

    // Server sync state and queue management
    struct ModelSyncState {
        mutable std::mutex mutex;
        mutable std::condition_variable cv;
        bool is_sync_running = false;
        bool is_full_sync = false;
        bool cancel_requested = false;
        uint64_t current_generation = 0;
        uint64_t completed_generation = 0;
        std::map<uint64_t, json> completed_generation_results;
        std::set<std::string> active_targets;
        std::set<std::string> pending_targets;
        std::vector<std::string> completed_targets;
        std::vector<std::string> models_up_to_date;
        std::map<std::string, std::string> failed_models;
        int checked_count = 0;
        std::string terminal_error;

        // Progress metrics for active model download
        std::string current_model;
        std::string current_file;
        int file_index = 0;
        int total_files = 0;
        size_t bytes_downloaded = 0;
        size_t bytes_total = 0;
        int percent = 0;
    };
    mutable ModelSyncState sync_state_;
    std::function<void(const std::string&)> on_model_updated_cb_;
    std::function<void(const std::string&)> sync_phase_callback_;
    std::function<UpdateCheckResult(const std::vector<std::string>&)> update_check_override_;
    std::function<void(const std::string&, const json&, bool, DownloadProgressCallback)> download_model_override_;

    mutable std::map<std::string, ModelInfo> models_cache_;
    mutable std::map<std::string, std::string> public_model_aliases_;  // public name -> canonical name
    mutable std::map<std::string, std::string> canonical_public_names_;  // canonical name -> public name
    mutable std::map<std::string, std::string> filtered_out_models_;  // model_name -> filter reason
    // Real-backend recipes whose entire built-in model set was size-filtered
    // with no model left visible. Populated alongside filtered_out_models_.
    mutable std::set<std::string> recipes_all_models_filtered_;
    mutable bool cache_valid_ = false;

    // Refresh user_models.json on-demand when a user.* lookup misses the cache.
    // This keeps startup cache warmup / external registry writes from causing
    // stale hard "Model not found" failures for registered user models.
    bool refresh_user_models_from_disk_for_lookup(const std::string& model_name);

    json get_sync_status_locked() const;
    void rebuild_public_model_aliases_locked();
};

} // namespace lemon
