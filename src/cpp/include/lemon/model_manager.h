#pragma once

#include <stdexcept>
#include <cstdint>
#include <string>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <mutex>
#include <functional>
#include <memory>
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
    bool suggested = false;
    std::string source;  // Local origin: local_upload/local_path/extra_models_dir
    std::string registry_source = "huggingface";  // Remote registry: huggingface/modelscope
    bool downloaded = false;     // Whether model is downloaded and available
    bool update_available = false; // Whether a newer remote-registry version exists
    double size = 0.0;   // Model size in GB
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
    // pricing in /v1/models). Used for display only — never affects routing.
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

    // Wires the cloud provider registry. ModelManager uses it to look up
    // {base_url, api_key} per provider when refreshing cloud models during
    // build_cache(). Pointer (not ownership) — Server owns the registry.
    // Must be called before the first build_cache() / get_supported_models().
    void set_cloud_registry(CloudProviderRegistry* registry);

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

    // Get all supported models from server_models.json
    std::map<std::string, ModelInfo> get_supported_models();

    // Get downloaded models
    std::map<std::string, ModelInfo> get_downloaded_models();

    // Filter models by available backends
    std::map<std::string, ModelInfo> filter_models_by_backend(
        const std::map<std::string, ModelInfo>& models);

    // Register a user model
    void register_user_model(const std::string& model_name,
                            const json& model_data,
                            const std::string& source = "");

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

    // Check if model is downloaded
    bool is_model_downloaded(const std::string& model_name);

    // Check all downloaded models for updates in their configured remote registry.
    // Fetches the latest commit SHA for each model's repo and compares it
    // with the cached commit (refs/main). Sets update_available on models
    // whose upstream repo has changed and clears stale flags for repos that
    // were successfully verified as current. Returns public model names with
    // updates available.
    // Safe to call from a background thread — locks are internal.
    std::vector<std::string> check_for_model_updates();

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

    void start_directory_watcher();

private:
    // Cycle-detecting overload used by the collection fan-out in download_model.
    // `visited` accumulates collection names already entered on the current
    // call chain; re-entering one throws.
    void download_model(const std::string& model_name,
                       const json& model_data,
                       bool do_not_upgrade,
                       DownloadProgressCallback progress_callback,
                       std::set<std::string>& visited);

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

    // Cache management
    void build_cache();
    void add_model_to_cache(const std::string& model_name);
    void update_model_options_in_cache(const ModelInfo& info);
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

    json server_models_;
    json user_models_;
    json recipe_options_;
    json architecture_defaults_;  // Per-architecture recipe option overlays (from resources)
    std::string extra_models_dir_;  // Secondary directory for GGUF model discovery
    CloudProviderRegistry* cloud_registry_ = nullptr;  // Not owned
    std::unique_ptr<DirectoryWatcher> directory_watcher_;

    // Cache of all models with their download status
    mutable std::mutex models_cache_mutex_;

    // Serializes concurrent downloads that write into the same snapshot
    // (keyed by checkpoint repo). See download_registered_model.
    std::mutex download_locks_mutex_;
    std::map<std::string, std::shared_ptr<std::mutex>> download_locks_;

    // Prevent startup and manual update checks from running concurrently.
    std::mutex update_check_mutex_;

    mutable std::map<std::string, ModelInfo> models_cache_;
    mutable std::map<std::string, std::string> public_model_aliases_;  // public name -> canonical name
    mutable std::map<std::string, std::string> canonical_public_names_;  // canonical name -> public name
    mutable std::map<std::string, std::string> filtered_out_models_;  // model_name -> filter reason
    mutable bool cache_valid_ = false;

    // Refresh user_models.json on-demand when a user.* lookup misses the cache.
    // This keeps startup cache warmup / external registry writes from causing
    // stale hard "Model not found" failures for registered user models.
    bool refresh_user_models_from_disk_for_lookup(const std::string& model_name);

    void rebuild_public_model_aliases_locked();
};

} // namespace lemon
