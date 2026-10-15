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
#include "model_registry.h"
#include "model_types.h"
#include "models/alias_manager.h"
#include "models/catalog_descriptor.h"
#include "models/change_notifier.h"
#include "models/download_progress.h"
#include "models/ingest.h"
#include "models/registry_documents.h"
#include "models/recipe_options_store.h"
#include "models/sync_service.h"
#include "models/update_scan.h"
#include "recipe_options.h"

namespace lemon {

namespace backends {
enum class DownloadPolicy : int;
}  // namespace backends

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

// Parsed collection.router routing policy (defined in routing_policy.h). Only
// forward-declared here so this widely-included header stays light; ModelInfo
// holds it behind a shared_ptr, which supports incomplete types.
struct RoutePolicy;

namespace models {
class ArtifactSource;
class CatalogSet;
class ModelArtifactStore;
class ModelCatalog;
struct FilterReasons;
struct ResolvedModel;
struct StoreSnapshot;
}  // namespace models

// Image generation defaults for SD models
struct ImageDefaults {
    int steps = 20;
    float cfg_scale = 7.0f;
    int width = 512;
    int height = 512;
    std::string sampling_method;
    float flow_shift = 0.0f;
    std::uint64_t soft_pixel_cap = 0;  // 0 uses the server's inferred edit-size cap

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
    std::string registry_source;  // Remote registry: huggingface/modelscope; empty when unpinned
    bool downloaded = false;     // Whether model is downloaded and available
    bool update_available = false; // Whether a newer remote-registry version exists
    std::optional<bool> auto_update = std::nullopt; // Optional per-model auto-update override
    double size = 0.0;   // Model size in GB
    // Resident working set for a streaming backend; 0 = filter on full `size`.
    // See streaming_working_set_gb() / filter_models_by_backend.
    double min_resident_gb = 0.0;
    int64_t max_context_window = 0;  // Static model-supported text context, when known
    int64_t max_output_tokens = 0;    // Static model-supported completion/output token limit, when known

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

// Everything the composition root needs. Server fills it in; a test points the
// same struct at fixture paths and fixture catalogs.
struct ModelManagerConfig {
    std::string cache_dir;  // empty: whatever utils::get_cache_dir() resolves to
    std::string extra_models_dir;
    CloudProviderRegistry* cloud_providers = nullptr;  // Not owned
    std::function<std::string()> default_model_source;
    std::vector<std::shared_ptr<models::ModelCatalog>> extra_catalogs;
};

class ModelManager {
public:
    explicit ModelManager(ModelManagerConfig config);

    // The paths-only shorthand.
    explicit ModelManager(const std::string& extra_models_dir = "");

// Joins the watcher thread: its callback locks models_cache_mutex_, declared
// after directory_watcher_ and so freed first.
    ~ModelManager();

    std::map<std::string, ModelInfo> discover_extra_models_for_test() const {
        return discover_extra_models();
    }

    CloudProviderRegistry* cloud_registry() const { return cloud_registry_; }

    json model_sources();

    // The registry a pull of this model reads: what the model was registered with
    // wins over the server-wide default.
    RegistryId download_source_for(const ModelInfo& info) const;

    // The ArtifactSource for one checkpoint.
    std::unique_ptr<models::ArtifactSource> source_for(
        const ModelInfo& info, const std::string& checkpoint_type) const;

    // Unfiltered: see models::KnownModels.
    std::map<std::string, ModelInfo> all_models() const;

    // Drop the model's own registration, if it has one. Rows the catalog re-creates
    // have none.
    void forget_registered_model(const std::string& canonical_model_name);

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

// The two change signals; see models/change_notifier.h. Fires only when a routing
// policy could have changed.
    void on_models_changed(std::function<void(std::uint64_t)> subscriber);
    void on_model_updated(std::function<void(const std::string&)> subscriber);

    // Server also calls this once at startup, so a subscriber starts from an
    // authoritative set instead of waiting for the first change.
    void notify_models_changed();

    // Snapshot generation, for consumers that need currency not a callback.
    std::uint64_t generation();

    std::set<std::string> active_helper_models();

    // Get all supported models from server_models.json
    std::map<std::string, ModelInfo> get_supported_models();

    // Get downloaded models
    std::map<std::string, ModelInfo> get_downloaded_models();

// Filter models by available backends. Set track_recipe_availability only on the
// full-cache build. Callers must hold a StoreWrite: filter reasons change but are
// not published.
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

    // The rows of a listing: the models given, plus one row per alias that resolves
    // to one of them, keyed by the alias. Pointers into `listed`.
    std::vector<std::pair<std::string, const ModelInfo*>> listing_rows(
        const std::map<std::string, ModelInfo>& listed);

    // One requested name to one model: normalize, snapshot precedence, then the
    // alias map. Every surface that accepts a model name answers through this.
    std::optional<models::ResolvedModel> resolve(const std::string& requested);

    // A requested alias settles to the model it points at; anything else passes
    // through. Every surface that acts on a model name goes through this.
    std::string canonical_for_request(const std::string& requested);

    // Aliases: one map, held here, delegated everywhere.
    std::map<std::string, std::string> aliases() const;
    bool set_alias(const std::string& alias, const std::string& target, std::string& err_msg);
    bool remove_alias(const std::string& alias);
    std::optional<std::string> resolve_alias(const std::string& alias_or_name) const;
    bool has_alias(const std::string& alias) const;

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

// Recipes that lost every model to the memory heuristic and kept none visible.

    // Test-only raw view of the side table without a cache rebuild; prefer
    // recipes_with_all_models_filtered() everywhere else.
    std::set<std::string> recipes_all_models_filtered_snapshot() const;

    // Check if model is downloaded
    bool is_model_downloaded(const std::string& model_name);

    using UpdateCheckResult = models::UpdateScanResult;

    // Check downloaded models for updates in their configured remote registry.
    // Fetches the latest commit SHA for each model's repo and compares it
    // with the cached commit. Sets update_available on models whose upstream
    // repo has changed and clears stale flags for repos that were successfully
    // verified as current. If targets is non-empty, restricts check to specified targets.
    // Safe to call from a background thread — locks are internal.
    UpdateCheckResult check_for_model_updates(const std::vector<std::string>& targets = {});


    // Check if model should be automatically updated when updates are detected
    bool should_auto_update(const ModelInfo& info) const;

    // Register a callback triggered during synchronization phases (e.g. for deterministic unit testing).
    void set_sync_phase_callback(std::function<void(const std::string&)> cb) {
        sync_phase_callback_ = std::move(cb);
    }

    // Set update_available in cache for deterministic unit testing.
    void set_model_update_available_for_test(const std::string& model_name, bool available) {
        std::string canonical = resolve_model_name(model_name);
        StoreWrite store_write(*this);
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


    using SyncEnqueueResult = models::SyncService::Enqueued;

    // Query model sync status (running state, active/pending targets, completed count, or specific sync_id outcome)
    json get_sync_status(uint64_t sync_id = 0) const;

    // Synchronously queue targets for sync. Returns dispatch result with sync_id and whether sync was already in progress.
    SyncEnqueueResult enqueue_sync(const std::vector<std::string>& target_models = {}, bool attach_if_running = false);

    // Blocks until the generation in flight (or the last one queued) is done.
    json wait_for_sync();

    // Shutdown waits here, so a download cannot outlive the store it wrote.
    void wait_until_sync_idle();

    // Trigger sync/update of specified or all outdated models.
    // When target_models is empty, targets all downloaded outdated models.
    // If dry_run is true, returns update status without downloading files.
    json sync_models(const std::vector<std::string>& target_models = {}, bool dry_run = false, bool attach_if_running = false);


    // The artifact store's answer, exposed for the HTTP and Ollama layers.
    backends::DownloadPolicy download_policy(const ModelInfo& info) const;

    void populate_model_metadata(ModelInfo& info);

    // The artifact store's completeness answer for this model.
    bool checkpoints_complete(const ModelInfo& info) const;

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
    static json load_server_models();
    json load_architecture_defaults();

    // Remove a user model entry from user_models.json (no file deletion).
    void unregister_user_model(const std::string& model_name);

    static std::string get_user_models_file();
    std::string get_recipe_options_file();

    // The authored layer of build_recipe_options()'s ladder, read from the
    // registry row. Takes no store lock.
    json registry_recipe_options(const std::string& cache_key);
    json write_saved_model_options(const std::string& model_name, const json& options, bool merge);

    // Cache management
    void build_cache();
    void add_model_to_cache(const std::string& model_name);
    // Caller must hold models_cache_mutex_.
    void update_model_options_in_cache_locked(const ModelInfo& info);
    void update_model_in_cache(const std::string& model_name, bool downloaded);
    void remove_model_from_cache(const std::string& model_name);

    // Fills info.resolved_paths from the artifact store, adopting a complete
    // repository from the extra models directory when the managed cache lacks it.
    void resolve_all_model_paths(ModelInfo& info);

    // Discover independent GGUF models, excluding complete registered cache files.
    std::map<std::string, ModelInfo> discover_extra_models(
        const std::set<std::filesystem::path>& registered_paths = {}) const;

    models::RegistryDocuments registries_;
    RecipeOptionsStore recipe_options_;
    json architecture_defaults_;  // Per-architecture recipe option overlays (from resources)
    ModelManagerConfig config_;
    std::string extra_models_dir_;  // Secondary directory for GGUF model discovery
    models::AliasManager alias_manager_;
    CloudProviderRegistry* cloud_registry_ = nullptr;  // Not owned
    std::function<std::string()> default_model_source_provider_;
    std::unique_ptr<DirectoryWatcher> directory_watcher_;

    models::ModelChangeNotifier change_notifier_;

    // Serializes writers of the model set. Writers use StoreWrite, which
    // publishes for them; snapshot readers need no lock.
    mutable std::mutex models_cache_mutex_;
    // Serializes an options write end to end: store mutation plus the matching
    // cache patch. The store orders its own state; this keeps the two in step so
    // two concurrent writers cannot leave the cache holding the earlier entry.
    std::mutex recipe_options_write_mutex_;

    std::function<void(const std::string&)> sync_phase_callback_;
    std::function<UpdateCheckResult(const std::vector<std::string>&)> update_check_override_;
    std::function<void(const std::string&, const json&, bool, DownloadProgressCallback)> download_model_override_;

    std::unique_ptr<models::CatalogSet> catalog_set_;
    mutable std::map<std::string, ModelInfo> models_cache_;
    mutable std::map<std::string, std::string> public_model_aliases_;  // public name -> canonical name
    mutable std::map<std::string, std::string> canonical_public_names_;  // canonical name -> public name
// Why each model was hidden, and which recipes lost their whole set.
    std::shared_ptr<const models::FilterReasons> hardware_filter_;

    // Immutable view of the fields above. Written under models_cache_mutex_,
    // read with atomic load/store rather than under it.
    std::shared_ptr<const models::StoreSnapshot> store_snapshot_;
    std::uint64_t store_generation_ = 0;

    // Requires models_cache_mutex_ held; StoreWrite calls it on scope exit.
    void publish_store_locked();

  public:
// Locks the store and publishes on release, so a mutating path cannot forget to
// publish and readers never see a half-built set.
    class StoreWrite {
      public:
        explicit StoreWrite(ModelManager& manager);
        StoreWrite(const StoreWrite&) = delete;
        StoreWrite& operator=(const StoreWrite&) = delete;
        ~StoreWrite();

      private:
        ModelManager& manager_;
        std::unique_lock<std::mutex> lock_;
    };

  private:
    mutable bool cache_valid_ = false;

    UpdateCheckResult run_update_scan(const std::vector<std::string>& targets);
    models::SyncService::Deps make_sync_deps();
    models::ModelIngest::Deps make_ingest_deps();

    void publish_update_scan(const models::ScanDetermination& settled,
                             models::UpdateScanResult& result);
    void rebuild_public_model_aliases_locked();

    // Declared last so it dies first: the worker joins before the store it
    // publishes to does.
    const std::unique_ptr<models::ModelArtifactStore> artifacts_;
    // Fetches registry bytes; also the ArtifactFetchService the backends ask.
    std::unique_ptr<models::ModelIngest> ingest_;
    models::SyncService sync_;
};

} // namespace lemon
