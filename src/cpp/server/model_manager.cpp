#include <lemon/model_manager.h>
#include <lemon/runtime_config.h>
#include <lemon/hf_variants.h>
#include <lemon/model_registry.h>
#include <lemon/models/artifact_resolver.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/catalog.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/models/scan_catalog.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/models/model_index.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>
#include <lemon/system_info.h>
#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/backends/backend_utils.h>
#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/backends/hf_cache_util.h>
#include <lemon/cloud_provider_registry.h>
#include <lemon/gguf_shard_utils.h>
#include <lemon/registry_files.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>
#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;
using namespace lemon::models;



namespace lemon {

// Properties which are defined by the user for model registration.
static const std::vector<std::string> USER_DEFINED_MODEL_PROPS = std::vector<std::string>{
    "checkpoints", "checkpoint", "recipe", "mmproj", "size",
    "image_defaults", "audio_defaults", "components", "recipe_options",
    "routing", "system_prompt", "version", "source", "registry_source",
    "auto_update"
};


static constexpr const char USER_MODEL_PREFIX[] = "user.";
static constexpr size_t USER_MODEL_PREFIX_LEN = sizeof(USER_MODEL_PREFIX) - 1;



// Built-ins are keyed bare in models_cache_; user.* and extra.* keys already
// include their canonical prefix. This helper returns the canonical ID for any
// cache key, which is the form used by recipe_options.json on disk.
static std::string cache_key_to_canonical_id(const std::string& cache_key) {
    if (parse_canonical_id(cache_key)) {
        return cache_key;
    }
    return canonical_id(ModelSource::Builtin, cache_key);
}

// An illegal label set names the model it came from, wherever it is reported —
// a /pull 400, a collection import refusal, a skipped entry on load.
static std::string describe_illegal_labels(const std::string& model_name,
                                           const std::string& reason) {
    return "Model '" + model_name + "': " + reason;
}

// Candidate roots that FLM may use to store models. FLM resolves its model
// directory from the FLM_MODEL_PATH env var (set by the installer) and falls
// back to a built-in default that has changed across releases. lemond is often
// launched from a parent process that predates the FLM install and therefore
// doesn't see FLM_MODEL_PATH, so we also probe every documented default.
// Order is most-specific to most-historical.

void ModelManager::populate_model_metadata(ModelInfo& info) {
    // A route to someone else's GPU has no local bytes to read metadata from.
    if (download_policy(info) == backends::DownloadPolicy::None) {
        return;
    }
    info.max_context_window = 0;
    if (!info.downloaded) return;

    // Per-backend metadata (GGUF arch/labels for llamacpp, config.json ctx for
    // flm, …) is read by the backend's ops, not a recipe switchboard here.
    backends::BackendOpsContext ctx;
    backends::ops_for(info.recipe)->populate_metadata(info, ctx);
}

static bool is_user_model_name(const std::string& model_name) {
    return model_name.rfind(USER_MODEL_PREFIX, 0) == 0;
}

static bool is_extra_model_name(const std::string& model_name) {
    return model_name.rfind(models::extra_model_prefix, 0) == 0;
}

static std::string strip_user_model_prefix(const std::string& model_name) {
    if (is_user_model_name(model_name)) {
        return model_name.substr(USER_MODEL_PREFIX_LEN);
    }
    return model_name;
}

// The registry row a cache key was minted from, null when there is none; the pin
// must outlive the pointer. Deliberately not parse_canonical_id(): a key like
// "builtin.X" has no row, which is what every registry lookup has always done.
static const json* registry_row(const models::RegistryDocuments::Pin& rows,
                                const std::string& cache_key) {
    return rows.row(is_user_model_name(cache_key) ? models::RegistryDocument::User
                                                  : models::RegistryDocument::Builtin,
                    strip_user_model_prefix(cache_key));
}

void ModelManager::set_default_model_source_provider(std::function<std::string()> provider) {
    default_model_source_provider_ = std::move(provider);
}

RegistryId ModelManager::download_source_for(const ModelInfo& info) const {
    if (!info.registry_source.empty()) {
        return parse_remote_registry_source(info.registry_source);
    }
    if (default_model_source_provider_) {
        return parse_remote_registry_source(default_model_source_provider_());
    }
    return models::huggingface_descriptor().id;
}

static std::string repo_id_to_cache_dir_name(const std::string& repo_id,
                                             const std::string& registry_source) {
    return registry_repo_cache_dir_name(repo_id,
        parse_remote_registry_source(registry_source));
}







// Default device for a recipe: the backend descriptor is authoritative for
// registered backends; collection/unknown recipes fall back to the recipe map.
// (A backend whose device depends on the chosen backend variant resolves the
// final device at load time via WrappedServer::effective_device.)
static DeviceType device_type_for_recipe(const std::string& recipe) {
    if (const auto* desc = lemon::backends::descriptor_for(recipe)) {
        return desc->default_device;
    }
    return get_device_type_from_recipe(recipe);
}

// Fold the model-level precedence layers, lowest first: the document's image
// defaults, the row's authored `recipe_options`, then what the user saved. The
// full ladder is documented in SDServer::build_extra_args().
// json_recipe_options: pre-extracted recipe_options for this model (from build_cache's
// two-phase pattern). Pass a null json if the model JSON should be read directly instead.
// saved_recipe_options_key: canonical ID (user.* / extra.* / builtin.*) under which the
// user's saved options are keyed in recipe_options.json. Translate from the cache key with
// cache_key_to_canonical_id() before calling.
static RecipeOptions build_recipe_options(const ModelInfo& info,
                                          const json& json_recipe_options,
                                          const std::string& saved_recipe_options_key,
                                          const json& saved_recipe_options) {
    // Non-sd recipes pass an empty image_defaults layer.
    json image_defaults = json::object();

    if (info.image_defaults.has_defaults) {
        image_defaults["steps"] = info.image_defaults.steps;
        image_defaults["cfg_scale"] = info.image_defaults.cfg_scale;
        image_defaults["width"] = info.image_defaults.width;
        image_defaults["height"] = info.image_defaults.height;
        if (!info.image_defaults.sampling_method.empty())
            image_defaults["sampling_method"] = info.image_defaults.sampling_method;
        if (info.image_defaults.flow_shift > 0.0f)
            image_defaults["flow_shift"] = info.image_defaults.flow_shift;
    }

    json user_saved = JsonUtils::has_key(saved_recipe_options, saved_recipe_options_key)
                          ? saved_recipe_options[saved_recipe_options_key]
                          : json(nullptr);

    return RecipeOptions::merge_precedence_layers(
        info.recipe, image_defaults, json_recipe_options, user_saved);
}




std::uintmax_t sharded_gguf_size_bytes(const fs::path& shard_path) {
    std::string base;
    int total = 0;
    if (!is_gguf_shard_filename(shard_path.filename().string(), &base, &total)) {
        return 0;
    }

    const fs::path dir = shard_path.parent_path();
    if (!safe_is_directory(dir)) {
        return 0;
    }

    uintmax_t sum = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, safe_dir_options, ec)) {
        if (!entry.is_regular_file(ec)) {
            if (ec) ec.clear();
            continue;
        }
        if (!same_shard_family(entry.path().filename().string(), base, total)) {
            continue;
        }

        auto size = fs::file_size(entry.path(), ec);
        if (!ec) {
            sum += size;
        } else {
            ec.clear();
        }
    }
    return sum;
}


// Replace the static registry size with the aggregate on-disk size once the
// files exist, so directory-checkpoint models (whose repos can carry more than
// the registry estimate) report what was actually downloaded. A sharded GGUF
// resolves to a single shard, which for unsloth-style layouts is a small stub,
// so the whole family is summed instead.
static void refresh_on_disk_size(ModelInfo& info) {
    uintmax_t total_size = 0;
    for (auto& [type, path] : info.resolved_paths) {
        (void)type;
        fs::path resolved = path_from_utf8(path);
        uintmax_t shard_total = sharded_gguf_size_bytes(resolved);
        total_size += (shard_total > 0) ? shard_total : models::ModelArtifactStore::size_bytes(resolved);
    }
    if (total_size == 0) {
        return;
    }
    double file_size_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);
    if (file_size_gb < 1.0) {
        info.size = std::round(file_size_gb * 1000) / 1000;
    } else if (file_size_gb < 10.0) {
        info.size = std::round(file_size_gb * 100) / 100;
    } else {
        info.size = std::round(file_size_gb * 10) / 10;
    }
}

std::vector<ModelFileInfo> ModelManager::list_model_files(const std::string& model_name) {
    return artifacts_->list_files(get_model_info(model_name));
}









void ModelManager::publish_store_locked() {
    auto next = std::make_shared<models::StoreSnapshot>();
    next->models = models_cache_;
    next->public_model_aliases = public_model_aliases_;
    next->canonical_public_names = canonical_public_names_;
    next->reasons = hardware_filter_;
    next->helper_models = collect_helper_models(models_cache_);
    next->valid = cache_valid_;
    next->generation = ++store_generation_;
    std::atomic_store(&store_snapshot_,
                      std::const_pointer_cast<const models::StoreSnapshot>(next));
}

ModelManager::StoreWrite::StoreWrite(ModelManager& manager)
    : manager_(manager), lock_(manager.models_cache_mutex_) {}

ModelManager::StoreWrite::~StoreWrite() {
    // Copying the model set can fail; readers keep the previous snapshot rather
    // than unwinding an exception out of a destructor.
    try {
        manager_.publish_store_locked();
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Could not publish model store: " << e.what() << std::endl;
    }
    lock_.unlock();
}

// Scanning and syncing are the parts that read the store and write bytes, so
// every dependency reaches back into the manager; the queue holds no model
// state of its own.
models::SyncService::Deps ModelManager::make_sync_deps() {
    models::SyncService::Deps deps;
    deps.scan = [this](const std::vector<std::string>& targets) { return run_update_scan(targets); };
    deps.download = [this](const std::string& name, const json& options, bool do_not_upgrade,
                           DownloadProgressCallback progress) {
        download_model(name, options, do_not_upgrade, progress);
    };
    deps.model_info = [this](const std::string& canonical) { return get_model_info(canonical); };
    deps.resolve_name = [this](const std::string& name) { return resolve_model_name(name); };
    deps.public_name = [this](const std::string& canonical) { return get_public_model_name(canonical); };
    deps.phase = [this](const std::string& phase) {
        if (sync_phase_callback_) {
            sync_phase_callback_(phase);
        }
    };
    return deps;
}

ModelManager::ModelManager(const std::string& extra_models_dir)
    : extra_models_dir_(extra_models_dir),
      registries_(load_server_models(), get_user_models_file()),
      recipe_options_(get_recipe_options_file()),
      artifacts_(std::make_unique<models::ModelArtifactStore>(models::ModelArtifactStore::Hooks{
          [this] { return get_hf_cache_dir(); },
          [this](const ModelInfo& info, const std::string& type) { return source_for(info, type); },
          [this] { return extra_models_dir_; },
      })),
      sync_(make_sync_deps()) {
    // Readers must always have a snapshot; an invalid one tells them to build.
    std::atomic_store(&store_snapshot_, std::shared_ptr<const models::StoreSnapshot>(
                                            std::make_shared<models::StoreSnapshot>()));
    architecture_defaults_ = load_architecture_defaults();

    // Releases before canonical IDs keyed built-in options by bare name; the
    // builtin. prefix keeps a shadowed built-in addressable.
    const models::RegistryDocuments::Pin builtins = registries_.pin();
    recipe_options_.migrate([&builtins](const std::string& bare_name) {
        return builtins.contains(models::RegistryDocument::Builtin, bare_name);
    });

    if (!extra_models_dir_.empty()) {
        LOG(INFO, "ModelManager") << "Extra models directory set to: " << extra_models_dir_ << std::endl;
        start_directory_watcher();
    }
}

std::string ModelManager::get_user_models_file() {
    return get_config_dir() + "/user_models.json";
}

std::string ModelManager::get_recipe_options_file() {
    return get_config_dir() + "/recipe_options.json";
}

std::string ModelManager::get_hf_cache_dir() const {
    return lemon::utils::get_hf_cache_dir();
}

void ModelManager::invalidate_models_cache() {
    StoreWrite store_write(*this);
    cache_valid_ = false;
}

void ModelManager::on_models_changed(std::function<void(std::uint64_t)> subscriber) {
    change_notifier_.on_models_changed(std::move(subscriber));
}

void ModelManager::on_model_updated(std::function<void(const std::string&)> subscriber) {
    change_notifier_.on_model_updated(std::move(subscriber));
}

std::uint64_t ModelManager::generation() {
    return std::atomic_load(&store_snapshot_)->generation;
}

std::set<std::string> ModelManager::active_helper_models() {
    build_cache();
    return std::atomic_load(&store_snapshot_)->helper_models;
}

void ModelManager::notify_models_changed() {
    // The snapshot a subscriber must read has to exist first. See
    // models/change_notifier.h for what the generation means to it.
    build_cache();
    change_notifier_.models_changed(std::atomic_load(&store_snapshot_)->generation);
}

void ModelManager::set_extra_models_dir(const std::string& dir) {
    extra_models_dir_ = dir;

    directory_watcher_.reset();

    if (!extra_models_dir_.empty()) {
        LOG(INFO, "ModelManager") << "Extra models directory set to: " << extra_models_dir_ << std::endl;
        start_directory_watcher();
    }

    {
        StoreWrite store_write(*this);
        cache_valid_ = false;
    }

    notify_models_changed();
}

ModelManager::~ModelManager() {
    directory_watcher_.reset();
}

void ModelManager::start_directory_watcher() {
    directory_watcher_ = std::make_unique<DirectoryWatcher>(extra_models_dir_);
    directory_watcher_->set_callback([this]() {
        LOG(DEBUG, "ModelManager") << "Extra models directory changed, invalidating cache" << std::endl;
        {
            StoreWrite store_write(*this);
            cache_valid_ = false;
        }
        notify_models_changed();
    });
    directory_watcher_->start();
}

// Apply the default only after discovery has checked for an explicit directory mode.



std::map<std::string, ModelInfo> ModelManager::discover_extra_models(
    const std::set<fs::path>& registered_paths) const {
    return models::ScanCatalog(extra_models_dir_, device_type_for_recipe(extra_model_recipe))
        .enumerate(registered_paths);
}

std::unique_ptr<models::ArtifactSource> ModelManager::source_for(
    const ModelInfo& info, const std::string& checkpoint_type) const {
    models::ArtifactSourceResolver::Hooks hooks;
    hooks.download_policy = [this](const ModelInfo& info) { return download_policy(info); };

    hooks.default_registry_id = [this]() {
        return default_model_source_provider_ != nullptr ? default_model_source_provider_()
                                                        : std::string();
    };
    return models::ArtifactSourceResolver(hooks).source_for(info, checkpoint_type);
}

void ModelManager::resolve_all_model_paths(ModelInfo& info) {
    artifacts_->resolve_all(info);
}

json ModelManager::load_server_models() {
    try {
        // Load from resources directory (relative to executable)
        std::string models_path = get_resource_path("resources/server_models.json");
        return JsonUtils::load_from_file(models_path);
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Failed to load server_models.json: " << e.what() << std::endl;
        LOG(ERROR, "ModelManager") << "This is a critical file required for the application to run." << std::endl;
        LOG(ERROR, "ModelManager") << "Executable directory: " << get_executable_dir() << std::endl;
        throw std::runtime_error("Failed to load server_models.json");
    }
}

json ModelManager::load_architecture_defaults() {
    try {
        std::string path = get_resource_path("resources/architecture_defaults.json");
        return JsonUtils::load_from_file(path);
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Could not load architecture_defaults.json: " << e.what() << std::endl;
        return json::object();
    }
}

json ModelManager::get_architecture_defaults(const std::string& architecture) const {
    if (architecture.empty() || !architecture_defaults_.is_object()) {
        return json::object();
    }
    if (architecture_defaults_.contains(architecture) &&
        architecture_defaults_[architecture].is_object()) {
        return architecture_defaults_[architecture];
    }
    return json::object();
}

void ModelManager::save_model_options(const ModelInfo& info) {
    LOG(INFO, "ModelManager") << "Saving options for model: " << info.model_name << std::endl;
    // Persist under canonical ID (built-ins are keyed bare in cache but
    // recipe_options.json stores them as builtin.<name>).
    const std::string id = cache_key_to_canonical_id(info.model_name);
    std::lock_guard<std::mutex> write_lock(recipe_options_write_mutex_);

    recipe_options_.replace(id, info.recipe_options.to_json());

    StoreWrite store_write(*this);
    update_model_options_in_cache_locked(info);
}

json ModelManager::get_saved_model_options(const std::string& model_name) {
    const std::string id = cache_key_to_canonical_id(resolve_model_name(model_name));
    return recipe_options_.entry(id);
}

RecipeOptions ModelManager::get_model_default_options(const ModelInfo& info) {
    return build_recipe_options(info, registry_recipe_options(resolve_model_name(info.model_name)),
                                "", json::object());
}

RecipeOptions ModelManager::preview_saved_model_options(const ModelInfo& info, const json& changes) {
    const std::string cache_key = resolve_model_name(info.model_name);
    const std::string id = cache_key_to_canonical_id(cache_key);

    json saved = get_saved_model_options(cache_key);
    for (const auto& [key, value] : changes.items()) {
        if (value.is_null()) {
            saved.erase(key);
        } else {
            saved[key] = value;
        }
    }
    // Same layering as write_saved_model_options, so a preview cannot differ
    // from the state the real write would produce.
    return build_recipe_options(info, registry_recipe_options(cache_key), id, json{{id, saved}});
}

json ModelManager::registry_recipe_options(const std::string& cache_key) {
    const models::RegistryDocuments::Pin rows = registries_.pin();
    const json* row = registry_row(rows, cache_key);
    return row ? models::row_recipe_options(*row) : json(nullptr);
}

json ModelManager::set_saved_model_options(const std::string& model_name, const json& saved) {
    return write_saved_model_options(model_name, saved, /*merge=*/false);
}

json ModelManager::update_saved_model_options(const std::string& model_name, const json& changes) {
    return write_saved_model_options(model_name, changes, /*merge=*/true);
}

// Read-modify-write of one model's recipe_options.json entry.
//
// recipe_options_write_mutex_ orders the whole-file rewrites, so two writers
// can't land their snapshots out of order; models_cache_mutex_ covers the merge
// and the matching cache update together, so the cache can never end up holding
// an older option set than the file. Only the first is held across disk I/O —
// every request thread contends on the cache mutex.
json ModelManager::write_saved_model_options(const std::string& model_name,
                                             const json& options, bool merge) {
    const std::string cache_key = resolve_model_name(model_name);
    const std::string id = cache_key_to_canonical_id(cache_key);
    LOG(INFO, "ModelManager") << "Updating saved options for model: " << model_name << std::endl;

    // get_model_info can take the store writer lock itself, since it rebuilds on
    // demand, so read it before locking. The registry layer is re-read under the
    // lock instead, since the rebuild below has to reflect the registry as it
    // stands at that moment.
    ModelInfo info;
    bool have_info = true;
    try {
        info = get_model_info(cache_key);
    } catch (const std::exception&) {
        have_info = false;
    }

    std::lock_guard<std::mutex> write_lock(recipe_options_write_mutex_);

    const json saved = merge ? recipe_options_.merge(id, options)
                             : recipe_options_.replace(id, options);

    if (have_info) {
        StoreWrite store_write(*this);
        // Same layering as build_cache(), so the two can't drift.
        info.recipe_options = build_recipe_options(
            info, registry_recipe_options(cache_key), id, json{{id, saved}});
        update_model_options_in_cache_locked(info);
    } else {
        invalidate_models_cache();
    }

    return saved;
}

std::map<std::string, ModelInfo> ModelManager::get_supported_models() {
    // Build cache if needed (lazy initialization)
    build_cache();

    // Return copy of cache (all models, including their download status)
    auto snapshot = std::atomic_load(&store_snapshot_);
    std::map<std::string, ModelInfo> public_models;
    for (const auto& [name, info] : snapshot->models) {
        std::string public_name = snapshot->public_name_for(name);
        if (public_name.empty()) {
            public_name = name;
        }
        ModelInfo public_info = info;
        public_info.model_name = public_name;
        public_models[public_name] = std::move(public_info);
    }
    return public_models;
}

ModelManager::UpdateCheckResult ModelManager::check_for_model_updates(const std::vector<std::string>& targets) {
    return sync_.scan_for_updates(targets);
}

// The scan lives in models/update_scan.cpp. What stays here is what the scan may
// not reach: the store it reads, the provenance file behind its fast path, and
// the one store write that publishes the flags it settled.
ModelManager::UpdateCheckResult ModelManager::run_update_scan(const std::vector<std::string>& targets) {
    models::UpdateScanDeps deps;
    deps.ensure_store = [this] { build_cache(); };
    deps.snapshot = [this] { return std::atomic_load(&store_snapshot_); };
    deps.hf_cache_dir = [this] { return get_hf_cache_dir(); };
    deps.registry_source = [](const ModelInfo& info) { return models::registry_source_of(info); };
    deps.processed_snapshot_id = [this](const fs::path& model_cache_dir,
                                        const std::string& model_name,
                                        const ModelInfo& info) {
        return artifacts_->processed_snapshot_id(model_cache_dir, info);
    };
    deps.cancel_requested = [this] { return sync_.cancel_requested(); };
    deps.scan_override = update_check_override_;
    deps.phase = sync_phase_callback_;
    deps.publish = [this](const models::ScanDetermination& settled,
                          models::UpdateScanResult& result) {
        publish_update_scan(settled, result);
    };

    return models::run_update_scan(deps, targets);
}

void ModelManager::publish_update_scan(const models::ScanDetermination& settled,
                                       models::UpdateScanResult& result) {
    StoreWrite store_write(*this);

    for (auto& [name, info] : models_cache_) {
        if (settled.verified.count(name) == 0) {
            continue;
        }
        auto pub_it = canonical_public_names_.find(name);
        const std::string pub_name =
            pub_it != canonical_public_names_.end() ? pub_it->second : name;
        if (result.failed_models.find(pub_name) != result.failed_models.end()) {
            continue;
        }
        info.update_available = settled.updated.count(name) != 0;
    }

    for (const auto& name : settled.updated) {
        auto pub_it = canonical_public_names_.find(name);
        const std::string pub_name =
            pub_it != canonical_public_names_.end() ? pub_it->second : name;
        if (result.failed_models.find(pub_name) == result.failed_models.end()) {
            result.updated_models.push_back(pub_name);
        }
    }

    for (const auto& name : settled.examined) {
        if (settled.verified.count(name) == 0 || settled.updated.count(name) != 0) {
            continue;
        }
        auto pub_it = canonical_public_names_.find(name);
        const std::string pub_name =
            pub_it != canonical_public_names_.end() ? pub_it->second : name;
        if (result.failed_models.find(pub_name) == result.failed_models.end()) {
            result.up_to_date_models.push_back(pub_name);
        }
    }
}

bool ModelManager::should_auto_update(const ModelInfo& info) const {
    return sync_.should_auto_update(info);
}

json ModelManager::get_sync_status(uint64_t sync_id) const {
    return sync_.status(sync_id);
}

void ModelManager::cancel_sync() {
    sync_.cancel();
}

void ModelManager::wait_until_sync_idle() {
    sync_.wait_until_idle();
}

ModelManager::SyncEnqueueResult ModelManager::enqueue_sync(const std::vector<std::string>& target_models, bool attach_if_running) {
    return sync_.enqueue(target_models, attach_if_running);
}

json ModelManager::wait_for_sync() {
    return sync_.wait_for(sync_.status().value("sync_id", 0));
}

json ModelManager::sync_models(const std::vector<std::string>& target_models, bool dry_run, bool attach_if_running) {
    if (dry_run) {
        LOG(INFO, "ModelManager") << "Checking models for sync/update (dry-run)..." << std::endl;
        UpdateCheckResult check_res = check_for_model_updates(target_models);

        std::vector<std::string> models_to_update = check_res.updated_models;
        std::vector<std::string> models_up_to_date = check_res.up_to_date_models;
        std::map<std::string, std::string> failed_models = check_res.failed_models;
        int checked_count = check_res.up_to_date_models.size() + check_res.updated_models.size();

        if (!target_models.empty()) {
            for (const auto& input_name : target_models) {
                std::string canonical_name = resolve_model_name(input_name);
                std::string pub_name = get_public_model_name(canonical_name);
                if (failed_models.count(pub_name) || failed_models.count(input_name)) {
                    continue;
                }
                ModelInfo info;
                bool found = false;
                {
                    auto snapshot = std::atomic_load(&store_snapshot_);
                    if (const ModelInfo* cached = snapshot->find(canonical_name)) {
                        info = *cached;
                        found = true;
                    }
                }

                if (!found) {
                    failed_models[input_name] = "Model not found or registered";
                    continue;
                }

                if (!info.downloaded) {
                    failed_models[input_name] = "Model is not downloaded";
                    continue;
                }
            }
        }

        json status_info = get_sync_status();
        bool running = status_info.value("already_in_progress", false);
        return json{
            {"status", running ? "in_progress" : (failed_models.empty() ? "success" : "failed")},
            {"dry_run", true},
            {"already_in_progress", running},
            {"sync_id", status_info.value("sync_id", 0)},
            {"completed_sync_id", status_info.value("completed_sync_id", 0)},
            {"checked_count", checked_count},
            {"updated_count", models_to_update.size()},
            {"models_updated", models_to_update},
            {"models_up_to_date", models_up_to_date},
            {"failed_models", failed_models}
        };
    }

    SyncEnqueueResult enqueue_res = sync_.enqueue(target_models, attach_if_running);
    return sync_.wait_for(enqueue_res.sync_id);
}

static std::string join_conflict_parts(const std::vector<std::string>& parts) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "; ";
        result += parts[i];
    }
    return result;
}

static std::string requested_registry_source(const json& requested) {
    const std::string public_source = requested.value("source", std::string());
    const std::string explicit_source = requested.value("registry_source", std::string());

    std::string normalized_explicit;
    if (!explicit_source.empty()) {
        normalized_explicit = remote_registry_source_name(
            parse_remote_registry_source(explicit_source));
    }

    if (is_remote_registry_source(public_source)) {
        const std::string normalized_public = remote_registry_source_name(
            parse_remote_registry_source(public_source));
        if (!normalized_explicit.empty() && normalized_explicit != normalized_public) {
            throw std::invalid_argument(
                "Model source and registry_source identify different registries");
        }
        return normalized_public;
    }

    return normalized_explicit;
}

static std::string describe_registration_conflict(const ModelInfo& existing,
                                                  const json& requested) {
    std::vector<std::string> diffs;

    auto add_diff = [&diffs](const std::string& field,
                             const std::string& current_value,
                             const std::string& requested_value) {
        if (!requested_value.empty() && requested_value != current_value) {
            diffs.push_back(field + " (existing='" + current_value +
                            "', requested='" + requested_value + "')");
        }
    };

    std::string requested_main;
    if (requested.contains("checkpoints") && requested["checkpoints"].is_object()) {
        requested_main = requested["checkpoints"].value("main", std::string());
        for (const auto& [role, value] : requested["checkpoints"].items()) {
            if (!value.is_string()) continue;
            add_diff("checkpoint[" + role + "]",
                     existing.checkpoint(role),
                     value.get<std::string>());
        }
    } else {
        requested_main = requested.value("checkpoint", std::string());
        add_diff("checkpoint", existing.checkpoint(), requested_main);
    }

    const std::string requested_recipe = requested.value("recipe", std::string());
    add_diff("recipe", existing.recipe, requested_recipe);

    const std::string requested_source = requested_registry_source(requested);
    add_diff("source", models::registry_source_of(existing), requested_source);

    const std::string requested_mmproj = requested.value("mmproj", std::string());
    if (!requested_mmproj.empty()) {
        const std::string main_for_mmproj = requested_main.empty()
            ? existing.checkpoint()
            : requested_main;
        add_diff("mmproj",
                 existing.checkpoint("mmproj"),
                 models::legacy_mmproj_to_checkpoint(main_for_mmproj, requested_mmproj));
    }

    return join_conflict_parts(diffs);
}

// Check if all components of a collection model are downloaded.
static bool check_component_downloaded(const ModelInfo& info,
                                        const std::map<std::string, ModelInfo>& model_map) {
    if (info.components.empty()) return false;
    for (const auto& component_name : info.components) {
        auto it = model_map.find(component_name);
        if (it == model_map.end() || !it->second.downloaded) {
            return false;
        }
    }
    return true;
}

void ModelManager::fetch_registry_artifacts(const ModelInfo& info,
                                            DownloadProgressCallback progress) {
    download_from_registry(info, progress);
}

void ModelManager::download_from_huggingface_engine(
        const ModelInfo& info, DownloadProgressCallback progress_callback) {
    fetch_registry_artifacts(info, progress_callback);
}

// Every alias form rebuild_public_model_aliases_locked() computes -- bare
// name (by source precedence), builtin.<X>, and ModelInfo::input_aliases --
// as a pure function of an arbitrary model set, so it can also run against
// the pre-filter set (#2748): a component's alias must resolve the same way
// whether or not its model currently survives hardware filtering. See that
// function's own doc comment for what each field means.
struct ModelAliasMaps {
    std::map<std::string, std::string> public_model_aliases;
    std::map<std::string, std::string> canonical_public_names;
};

static ModelAliasMaps compute_model_alias_maps(const std::map<std::string, ModelInfo>& models) {
    ModelAliasMaps result;

    struct Entry {
        std::string cache_key;
        ModelSource source;
    };
    std::map<std::string, std::vector<Entry>> by_bare;

    for (const auto& [cache_key, info] : models) {
        ModelSource source;
        std::string bare;
        if (auto canon = parse_canonical_id(cache_key)) {
            source = canon->source;
            bare = canon->bare_name;
        } else {
            source = ModelSource::Builtin;
            bare = cache_key;
        }
        by_bare[bare].push_back({cache_key, source});
    }

    for (auto& [bare, entries] : by_bare) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return precedence_rank(a.source) < precedence_rank(b.source);
        });

        const Entry& winner = entries.front();
        result.public_model_aliases[bare] = winner.cache_key;

        const auto winner_it = models.find(winner.cache_key);
        const bool winner_is_prefixed_collection =
            winner.source != ModelSource::Builtin &&
            winner_it != models.end() &&
            is_model_collection_recipe(winner_it->second.recipe);
        result.canonical_public_names[winner.cache_key] =
            winner_is_prefixed_collection ? canonical_id(winner.source, bare) : bare;

        for (size_t i = 1; i < entries.size(); ++i) {
            const Entry& shadowed = entries[i];
            std::string canonical = canonical_id(shadowed.source, bare);
            result.canonical_public_names[shadowed.cache_key] = canonical;
            if (canonical != shadowed.cache_key) {
                result.public_model_aliases[canonical] = shadowed.cache_key;
            }
        }
    }

    for (const auto& [cache_key, _info] : models) {
        if (parse_canonical_id(cache_key)) continue;
        std::string canonical = canonical_id(ModelSource::Builtin, cache_key);
        result.public_model_aliases.try_emplace(canonical, cache_key);
    }

    auto source_for_cache_key = [](const std::string& cache_key) {
        if (auto canon = parse_canonical_id(cache_key)) {
            return canon->source;
        }
        return ModelSource::Builtin;
    };

    for (const auto& [cache_key, info] : models) {
        for (const auto& alias : info.input_aliases) {
            if (parse_canonical_id(alias)) {
                if (models.find(alias) == models.end()) {
                    result.public_model_aliases[alias] = cache_key;
                }
            } else {
                auto existing = result.public_model_aliases.find(alias);
                if (existing == result.public_model_aliases.end()) {
                    result.public_model_aliases[alias] = cache_key;
                    continue;
                }

                if (source_for_cache_key(existing->second) == ModelSource::Builtin) {
                    std::string builtin_canonical = canonical_id(ModelSource::Builtin, alias);
                    result.canonical_public_names[existing->second] = builtin_canonical;
                    result.public_model_aliases[builtin_canonical] = existing->second;
                    result.public_model_aliases[alias] = cache_key;
                }
            }
        }
    }

    return result;
}

// Both routing-policy parse sites resolve a component the same way: alias
// first, falling back to the name as authored; then a primary type lookup
// with one fallback tier behind it. #2748 was that shape drifting between
// them, so they are built from here rather than hand-written twice. Only the
// tiers differ -- which model set each consults, and what the fallback is.
struct RoutingResolutionTiers {
    std::function<std::optional<std::string>(const std::string&)> alias;
    std::function<std::optional<ModelType>(const std::string&)> type;
    std::function<std::optional<ModelType>(const std::string&)> type_fallback;
};

static RoutingPolicyParseOptions make_routing_parse_options(RoutingResolutionTiers tiers) {
    RoutingPolicyParseOptions options;
    options.resolve_component =
        [tiers](const std::string& name) -> std::optional<std::string> {
        if (tiers.alias) {
            if (auto resolved = tiers.alias(name)) return *resolved;
        }
        // An unresolvable name stays as authored so component-role validation
        // can still report it by the name the author actually wrote.
        return name;
    };
    options.get_model_type = [tiers](const std::string& name) -> std::optional<ModelType> {
        if (tiers.type) {
            if (auto type = tiers.type(name)) return type;
        }
        return tiers.type_fallback ? tiers.type_fallback(name) : std::nullopt;
    };
    return options;
}

void ModelManager::build_cache() {
    // Probed before taking the writer: publishing on a read that changes nothing
    // would leave the generation useless as a change signal.
    {
        std::lock_guard<std::mutex> probe(models_cache_mutex_);
        if (cache_valid_) {
            return;
        }
    }

    StoreWrite store_write(*this);

    if (cache_valid_) {
        return;
    }

    LOG(INFO, "ModelManager") << "Building models cache..." << std::endl;

    models_cache_.clear();
    // Collected as rows parse so build_recipe_options() never re-reads a document.
    std::map<std::string, json> json_recipe_options;

    std::vector<std::unique_ptr<models::ModelCatalog>> catalogs;

    // What a row says about itself is parsed in models/json_catalog.cpp; these
    // are the facts it cannot answer.
    models::RegistryRowRules rows;
    rows.device_for_recipe = [](const std::string& recipe) {
        return device_type_for_recipe(recipe);
    };
    rows.stamp_labels = [](ModelInfo& info) {
        std::string illegal =
            lemon::backends::illegal_deployment_labels(info.labels, info.recipe);
        if (!illegal.empty()) {
            return describe_illegal_labels(info.model_name, illegal);
        }
        lemon::backends::ensure_deployment_label(info.labels, info.recipe);
        return std::string();
    };
    rows.populate_components = [this](ModelInfo& info) {
        populate_collection_components_from_cache(info);
    };
    rows.resolve_paths = [this](ModelInfo& info) { resolve_all_model_paths(info); };
    rows.recipe_options = [&json_recipe_options](const std::string& name, const json& authored) {
        json_recipe_options[name] = authored;
    };

    // Every source of models registers here, in the order that decides a claimed
    // name. The two JSON registries author theirs; everything after them keeps an
    // existing entry.
    const models::RegistryDocuments::Pin documents = registries_.pin();

    models::RegistryRowRules builtin_rows = rows;
    builtin_rows.source_label = "built-in model";
    catalogs.push_back(std::make_unique<models::JsonCatalog>(
        models::CatalogDescriptor{"server_models.json"}, documents.builtin,
        std::move(builtin_rows)));

    // Registrations are keyed bare on disk and "user.<name>" in the cache.
    models::RegistryRowRules user_rows = rows;
    user_rows.name_prefix = "user.";
    user_rows.suggested_default = true;
    user_rows.source_label = "user model";
    catalogs.push_back(std::make_unique<models::JsonCatalog>(
        models::CatalogDescriptor{"user_models.json"}, documents.user,
        std::move(user_rows)));

    // The sources that scan or list rather than read a JSON document. A scanning
    // or listing source keeps what an earlier one claimed.

    // An --extra-models-dir scan plus GGUF identification. A registered model
    // stays attached to its catalog id when a complete repository cache also
    // lives in the directory, which also suppresses duplicates across cache
    // roots; unrelated GGUFs are still discovered.
    catalogs.push_back(std::make_unique<models::EnumeratedCatalog>(
        models::CatalogDescriptor{"extra_models_dir"},
        [this](const models::CatalogAccumulator& claimed) {
            std::set<fs::path> registered_extra_paths;
            if (!extra_models_dir_.empty()) {
                for (const auto& [name, info] : claimed.models()) {
                    ModelInfo candidate = info;
                    artifacts_->adopt_extra_cache(candidate, &registered_extra_paths);
                }
            }

            // External discovery is additive. A filesystem failure in this
            // optional source must never make built-in or user-registered
            // models disappear.
            std::vector<ModelInfo> discovered;
            try {
                for (auto& [name, info] : discover_extra_models(registered_extra_paths)) {
                    discovered.push_back(std::move(info));
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "ModelManager")
                    << "Extra model discovery failed; keeping registered models: " << e.what()
                    << std::endl;
            }
            return discovered;
        },
        [](const std::string& name) {
            LOG(INFO, "ModelManager") << "Warning: Discovered model '" << name
                                      << "' conflicts with another extra.* registration; skipping."
                                      << std::endl;
        }));

    // Dynamic discovery: backends whose models are supplied at runtime
    // (descriptor dynamic_models = true - flm from `flm list`, cloud from each
    // provider), each model carrying its own downloaded status. The backend is
    // reached through the catalog's callable, so nothing below this point needs
    // to know which backend contributed an entry.
    {
        const bool offline = [] {
            auto* cfg = RuntimeConfig::global();
            return cfg && cfg->offline();
        }();

        backends::BackendOpsContext octx;
        octx.fetch = this;
        octx.artifacts = artifacts_.get();
        octx.cloud_registry = cloud_registry_;

        for (const auto* desc : backends::all_descriptors()) {
            if (!desc->dynamic_models) {
                continue;
            }

            // A cloud provider is a listing, not a backend listing models from
            // its own runtime: one catalog per provider, so one provider's
            // failure costs only that provider's entries.
            if (desc->recipe == "cloud") {
                if (offline) {
                    LOG(DEBUG, "ModelManager")
                        << "Offline mode enabled, skipping cloud model listings" << std::endl;
                    continue;
                }
                if (cloud_registry_ != nullptr) {
                    for (const auto& rec : cloud_registry_->list_installed()) {
                        const std::string provider = rec.name;
                        catalogs.push_back(std::make_unique<models::ListedCatalog>(
                            provider, [this, provider] {
                                return backends::CloudServer::discover_provider_models(
                                    *cloud_registry_, provider);
                            }));
                    }
                }
                continue;
            }

            const backends::BackendOps* ops = backends::ops_for(desc->recipe);
            catalogs.push_back(std::make_unique<models::EnumeratedCatalog>(
                models::CatalogDescriptor{"discovered:" + desc->recipe},
                [ops, octx](const models::CatalogAccumulator&) {
                    return ops->discover_models(octx);
                }));
        }
    }

    models::CatalogAccumulator accumulated;
    for (const auto& catalog : catalogs) {
        catalog->enumerate(accumulated);
    }

    std::map<std::string, ModelInfo> all_models = accumulated.take();

    // Clients are not guaranteed to handle a model that declares no deployment
    // label. Every ingest path above stamps one; name any model that reached
    // here without.
    for (const auto& [name, info] : all_models) {
        ModelType mode = ModelType::LLM;
        if (!find_deployment_mode(info.labels, mode)) {
            LOG(WARNING, "ModelManager")
                << "Model '" << name << "' (recipe " << info.recipe
                << ") has no deployment label; add one (chat, transcription, embeddings, ...)"
                << std::endl;
        }
    }

    // One snapshot of the saved options for the whole pass, so a concurrent
    // options write cannot split the build across two states.
    const json saved_options = recipe_options_.all();
    for (auto& [name, info] : all_models) {
        json jro = json_recipe_options.count(name) ? json_recipe_options[name] : json(nullptr);
        info.recipe_options =
            build_recipe_options(info, jro, cache_key_to_canonical_id(name), saved_options);
        if (info.recipe_options.to_json().contains("auto_update") &&
            info.recipe_options.to_json()["auto_update"].is_boolean()) {
            info.auto_update = info.recipe_options.to_json()["auto_update"].get<bool>();
        }
    }

    // Pre-filter snapshots for the routing-policy resolver below (#2748); see
    // compute_model_alias_maps() for why the alias needs the full algorithm
    // rather than a plain bare-name map. Only router collections read them and
    // the alias rebuild is O(n log n) over the whole registry, so a host with
    // no router collection -- the common case, across ~15 build_cache() call
    // sites -- pays nothing.
    const bool needs_routing_snapshots =
        std::any_of(all_models.begin(), all_models.end(), [](const auto& entry) {
            return is_router_collection_recipe(entry.second.recipe);
        });

    std::map<std::string, ModelType> pre_filter_types;
    std::map<std::string, std::string> pre_filter_aliases;
    if (needs_routing_snapshots) {
        for (const auto& [name, info] : all_models) {
            pre_filter_types[name] = info.type;
        }
        pre_filter_aliases = compute_model_alias_maps(all_models).public_model_aliases;
    }

    // Step 2: Filter by backend availability. This is the full-registry pass, so
    // it also refreshes the recipe availability side table used to hide backends
    // that have nothing runnable on this host.
    all_models = filter_models_by_backend(all_models, /*track_recipe_availability=*/true);

    // Step 3: Check download status for all models. Dynamic-discovery backends
    // (flm, cloud) already set downloaded during discovery; everyone else asks
    // its backend ops (default = shared registry-cache completeness check).
    backends::BackendOpsContext status_ctx;
    status_ctx.fetch = this;
    status_ctx.artifacts = artifacts_.get();

    int downloaded_count = 0;
    // First pass: determine download status for non-collection models
    for (auto& [name, info] : all_models) {
        if (is_model_collection_recipe(info.recipe)) {
            continue;  // Handled in second pass after components are resolved
        }
        const auto* desc = backends::descriptor_for(info.recipe);
        if (!(desc && desc->dynamic_models)) {
            info.downloaded = backends::ops_for(info.recipe)->is_downloaded(info, status_ctx);
        }

        if (info.downloaded) {
            downloaded_count++;
        }
    }

    // Second pass: determine download status for collection models
    // (must happen after components have their downloaded status set)
    for (auto& [name, info] : all_models) {
        if (!is_model_collection_recipe(info.recipe)) continue;
        info.downloaded = check_component_downloaded(info, all_models);
        if (info.downloaded) {
            downloaded_count++;
        }
    }

    for (auto& [name, info] : all_models) {
        populate_model_metadata(info);
        if (info.downloaded && download_policy(info) != backends::DownloadPolicy::SelfManaged) {
            refresh_on_disk_size(info);
        }
        models_cache_[name] = info;
    }

    rebuild_public_model_aliases_locked();

    cache_valid_ = true;

    // Parse each collection.router model's routing policy while the cache and
    // alias map are built and the lock is held (avoids re-locking via
    // resolve_model_name()/get_model_info()). pre_filter_aliases is computed
    // over all_models, a strict superset of models_cache_, so it alone
    // covers everything public_model_aliases_ would (#2748).
    //
    // Components only the pre-filter tier can resolve are unavailable on this
    // host: a classifier bound to one takes its on_error path on every
    // request. Recorded here, where that is known, and reported per collection
    // below so the operator sees degraded routing instead of silence (#2748).
    std::set<std::string> unavailable_components;

    RoutingPolicyParseOptions policy_options = make_routing_parse_options({
        [&pre_filter_aliases](const std::string& name) -> std::optional<std::string> {
            auto it = pre_filter_aliases.find(name);
            if (it == pre_filter_aliases.end()) return std::nullopt;
            return it->second;
        },
        [this](const std::string& name) -> std::optional<ModelType> {
            auto it = models_cache_.find(name);
            if (it == models_cache_.end()) return std::nullopt;
            return it->second.type;
        },
        [&pre_filter_types, &unavailable_components](
            const std::string& name) -> std::optional<ModelType> {
            auto it = pre_filter_types.find(name);
            if (it == pre_filter_types.end()) return std::nullopt;
            unavailable_components.insert(name);
            return it->second;
        },
    });
    for (auto& [name, info] : models_cache_) {
        if (!is_router_collection_recipe(info.recipe)) {
            continue;
        }
        json doc;
        doc["recipe"] = info.recipe;
        doc["components"] = info.components;
        auto version_it = info.extras.find("version");
        if (version_it != info.extras.end()) {
            doc["version"] = version_it->second;
        }
        auto routing_it = info.extras.find("routing");
        if (routing_it != info.extras.end()) {
            doc["routing"] = routing_it->second;
        }
        unavailable_components.clear();
        try {
            info.route_policy = std::make_shared<const RoutePolicy>(
                parse_route_policy_collection(doc, policy_options));
        } catch (const std::exception& e) {
            LOG(WARNING, "ModelManager") << "Failed to parse routing policy for '"
                                         << name << "': " << e.what() << std::endl;
        }
        if (!unavailable_components.empty()) {
            std::ostringstream joined;
            bool first = true;
            for (const auto& component : unavailable_components) {
                if (!first) joined << ", ";
                joined << component;
                first = false;
            }
            LOG(WARNING, "ModelManager")
                << "Routing policy for '" << name << "' references component(s) "
                << "unavailable on this host: " << joined.str()
                << ". Any classifier bound to them will fall back to its on_error "
                << "result on every request." << std::endl;
        }
    }

    LOG(INFO, "ModelManager") << "Cache built: " << models_cache_.size()
              << " total, " << downloaded_count << " downloaded" << std::endl;
}

void ModelManager::add_model_to_cache(const std::string& model_name) {
    StoreWrite store_write(*this);

    if (!cache_valid_) {
        return; // Will initialize on next access
    }

    const bool is_user_model = is_user_model_name(model_name);
    const models::RegistryDocuments::Pin rows = registries_.pin();
    const json* model_json = registry_row(rows, model_name);

    if (!model_json) {
        LOG(WARNING, "ModelManager") << "'" << model_name << "' not found in JSON" << std::endl;
        return;
    }

    // Build ModelInfo
    ModelInfo info;
    info.model_name = model_name;
    models::parse_checkpoints(info, *model_json);
    models::parse_components(info, *model_json);
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.cloud_provider = JsonUtils::get_or_default<std::string>(*model_json, "cloud_provider", "");

    models::parse_image_defaults(info, *model_json);
    models::parse_extras(info, *model_json);
    info.recipe_options = build_recipe_options(info, models::row_recipe_options(*model_json),
                                               cache_key_to_canonical_id(model_name),
                                               recipe_options_.all());

    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", is_user_model);
    models::parse_model_source_fields(info, *model_json);
    info.system_prompt = JsonUtils::get_or_default<std::string>(*model_json, "system_prompt", "");

    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            info.labels.push_back(label.get<std::string>());
        }
    }
    std::string illegal =
        lemon::backends::illegal_deployment_labels(info.labels, info.recipe);
    if (!illegal.empty()) {
        LOG(ERROR, "ModelManager")
            << "Skipping " << describe_illegal_labels(model_name, illegal) << std::endl;
        return;
    }
    lemon::backends::ensure_deployment_label(info.labels, info.recipe);

    // Populate type and device fields (multi-model support)
    info.type = get_model_type_from_labels(info.labels);
    info.device = device_type_for_recipe(info.recipe);

    resolve_all_model_paths(info);

    // Check if it should be filtered out by backend availability
    std::map<std::string, ModelInfo> temp_map = {{model_name, info}};
    auto filtered = filter_models_by_backend(temp_map);

    if (filtered.empty()) {
        LOG(INFO, "ModelManager") << "Model '" << model_name << "' filtered out by backend availability" << std::endl;
        return; // Backend not available, don't add to cache
    }

    // Check download status (collections aggregate their components; everyone
    // else asks its backend ops).
    if (is_model_collection_recipe(info.recipe)) {
        info.downloaded = check_component_downloaded(info, models_cache_);
    } else {
        backends::BackendOpsContext octx;
        octx.fetch = this;
        octx.artifacts = artifacts_.get();
        info.downloaded = backends::ops_for(info.recipe)->is_downloaded(info, octx);
    }

    populate_model_metadata(info);
    models_cache_[model_name] = info;
    rebuild_public_model_aliases_locked();
    LOG(INFO, "ModelManager") << "Added '" << model_name << "' to cache (downloaded=" << info.downloaded << ")" << std::endl;
}

void ModelManager::update_model_options_in_cache_locked(const ModelInfo& info) {
    if (!cache_valid_) {
        return; // Will rebuild on next access
    }

    auto it = models_cache_.find(info.model_name);
    if (it != models_cache_.end()) {
        it->second.recipe_options = info.recipe_options;
    } else {
        LOG(WARNING, "ModelManager") << "'" << info.model_name << "' not found in cache" << std::endl;
    }
}

void ModelManager::update_model_in_cache(const std::string& model_name, bool downloaded) {
    StoreWrite store_write(*this);

    if (!cache_valid_) {
        return; // Will rebuild on next access
    }

    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        it->second.downloaded = downloaded;

        // After a fresh download the model is up to date
        if (downloaded) {
            it->second.update_available = false;
        }

        // Recompute resolved_path after download
        // The path changes now that files exist on disk
        if (downloaded) {
            resolve_all_model_paths(it->second);
            if (backends::ops_for(it->second.recipe)->invalidates_cache_after_download()) {
                cache_valid_ = false;
                LOG(INFO, "ModelManager") << "Invalidated model cache after download for '"
                          << model_name << "' (backend rebuilds its model list)" << std::endl;
                return;
            }
            populate_model_metadata(it->second);
            LOG(INFO, "ModelManager") << "Updated '" << model_name
                      << "' downloaded=" << downloaded
                      << ", resolved_path=" << it->second.resolved_path() << std::endl;
        } else {
            it->second.max_context_window = 0;
            LOG(INFO, "ModelManager") << "Updated '" << model_name
                      << "' downloaded=" << downloaded << std::endl;
        }
        refresh_on_disk_size(it->second);

        // Recompute downloaded status for any collections that
        // depend on this model, so the collection reflects component changes
        // without requiring a full cache rebuild.
        for (auto& [name, entry] : models_cache_) {
            if (!is_model_collection_recipe(entry.recipe)) continue;
            if (std::find(entry.components.begin(), entry.components.end(),
                          model_name) == entry.components.end()) {
                continue;
            }
            bool new_state = check_component_downloaded(entry, models_cache_);
            if (entry.downloaded != new_state) {
                entry.downloaded = new_state;
                LOG(INFO, "ModelManager") << "Collection '" << name
                          << "' downloaded=" << new_state << " (dependent on " << model_name << ")" << std::endl;
            }
        }
    } else {
        LOG(WARNING, "ModelManager") << "'" << model_name << "' not found in cache" << std::endl;
    }
}

void ModelManager::remove_model_from_cache(const std::string& model_name) {
    StoreWrite store_write(*this);

    if (!cache_valid_) {
        return;
    }

    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        // User models and local uploads should be removed entirely from cache
        // (they're not in server_models.json, so keeping them makes no sense)
        bool is_user_model = is_user_model_name(model_name);
        if (is_user_model || it->second.source == "local_upload") {
            models_cache_.erase(model_name);
            rebuild_public_model_aliases_locked();
            LOG(INFO, "ModelManager") << "Removed '" << model_name << "' from cache" << std::endl;
        } else {
            // Registered model - just mark as not downloaded
            it->second.downloaded = false;
            it->second.update_available = false;
            LOG(INFO, "ModelManager") << "Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}


std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    // Build cache if needed
    build_cache();

    // Filter and return downloaded models. Collections (Omni) are included once
    // all their components are downloaded: the server orchestrates /chat/completions
    // for them (see CollectionOrchestrator), so they are genuine OpenAI-compatible
    // chat models and should appear in /v1/models and Ollama /api/tags. A collection's
    // `downloaded` flag already reflects component status (see build_cache).
    auto snapshot = std::atomic_load(&store_snapshot_);
    std::map<std::string, ModelInfo> downloaded;
    for (const auto& [name, info] : snapshot->models) {
        if (info.downloaded) {
            std::string public_name = snapshot->public_name_for(name);
            if (public_name.empty()) {
                public_name = name;
            }
            ModelInfo public_info = info;
            public_info.model_name = public_name;
            downloaded[public_name] = std::move(public_info);
        }
    }
    return downloaded;
}

// Helper function to parse physical memory string (e.g., "32.00 GB") to GB as double
// Returns 0.0 if parsing fails
static double parse_physical_memory_gb(const std::string& memory_str) {
    if (memory_str.empty()) {
        return 0.0;
    }

    // Expected format: "XX.XX GB" or "XX GB"
    std::istringstream iss(memory_str);
    double value = 0.0;
    std::string unit;

    if (iss >> value >> unit) {
        // Convert to lowercase for comparison
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        if (unit == "gb") {
            return value;
        } else if (unit == "mb") {
            return value / 1024.0;
        } else if (unit == "tb") {
            return value * 1024.0;
        }
    }

    return 0.0;
}


double get_max_memory_of_device(json device, MemoryAllocBehavior mem_alloc_behavior) {
    // Get the maximum POSSIBLE accessible memory of the device in question,
    // taking into account the respective memory allocation behavior.

    double virtual_mem_gb = 0.0;
    double vram_gb = 0.0;

    if (device.contains("vram_gb")) {
        vram_gb = device["vram_gb"].get<double>();
    }
    if (device.contains("virtual_mem_gb"))
    {
        virtual_mem_gb = device["virtual_mem_gb"].get<double>();
    }

    switch (mem_alloc_behavior)
    {
    case MemoryAllocBehavior::Hardware:
        return vram_gb;

    case MemoryAllocBehavior::Virtual:
        return virtual_mem_gb;

    case MemoryAllocBehavior::Largest:
        return vram_gb > virtual_mem_gb ? vram_gb : virtual_mem_gb;

    case MemoryAllocBehavior::Unified:
        return virtual_mem_gb + vram_gb;

    default:
        return vram_gb;
    }
}

bool parse_TF_env_var(const char* env_var_name) {
    const char* env = std::getenv(env_var_name);
    return env && (std::string(env) == "1" ||
                   std::string(env) == "true" ||
                   std::string(env) == "TRUE" ||
                   std::string(env) == "yes");
}

// Resolves everything the hardware filter needs from configuration and system
// info. Keeping the reads here is what lets filter_hardware() stay a pure
// function of its inputs instead of reaching into backend and system headers.
static models::BackendAvailability probe_backend_availability(
    const std::map<std::string, ModelInfo>& models) {
    using namespace models;
    BackendAvailability availability;

    if (auto* cfg = lemon::RuntimeConfig::global()) {
        availability.filtering_disabled = cfg->disable_model_filtering();
        availability.assume_dgpu_gtt = cfg->enable_dgpu_gtt();
    }

    if (availability.filtering_disabled) {
        return availability;
    }

    if (availability.assume_dgpu_gtt) {
        LOG(INFO, "ModelManager") << "enable_dgpu_gtt has been set to true." << std::endl
                                  << "     Models are being filtered assuming GTT memory."
                                  << std::endl
                                  << "     Using GTT on a dGPU will have a significant "
                                     "performance impact."
                                  << std::endl;
    }

    json system_info = SystemInfoCache::get_system_info_with_cache();
    json hardware = system_info.contains("devices") ? system_info["devices"] : json::object();

    bool npu_available = hardware.contains("amd_npu") && hardware["amd_npu"].is_object() &&
                         hardware["amd_npu"].value("available", false);

    double largest_mem_pool_gb = 0.0;
    double curr_mem_pool_gb = 0.0;

    for (const auto& [dev_type, devices] : hardware.items()) {
        // Because we have mixed types this just makes every device_type an array.
        nlohmann::json dev_list = devices.is_array() ? devices : nlohmann::json{devices};

        for (const auto& dev : dev_list) {
            if (!dev.is_object())
                continue;

            // Behavior is chosen per device, not per device-type: AMD APUs are
            // reported under "amd_gpu" with "integrated": true and their GTT
            // pool in virtual_mem_gb -- an "amd_igpu" container key is never
            // emitted, so default to integrated when the flag is absent.
            MemoryAllocBehavior dev_mem_alloc_behavior = MemoryAllocBehavior::Hardware;
            if (dev_type == "amd_gpu" && dev.value("integrated", true))
                dev_mem_alloc_behavior = MemoryAllocBehavior::Largest;
            if (availability.assume_dgpu_gtt)
                dev_mem_alloc_behavior = MemoryAllocBehavior::Unified;
            curr_mem_pool_gb = get_max_memory_of_device(dev, dev_mem_alloc_behavior);
            largest_mem_pool_gb = largest_mem_pool_gb < curr_mem_pool_gb ? curr_mem_pool_gb
                                                                         : largest_mem_pool_gb;
        }
    }
    availability.largest_mem_pool_gb = largest_mem_pool_gb;

    double system_ram_gb = 0.0;
    if (system_info.contains("Physical Memory") && system_info["Physical Memory"].is_string()) {
        system_ram_gb = parse_physical_memory_gb(system_info["Physical Memory"].get<std::string>());
    }
    availability.system_ram_gb = system_ram_gb;

    if (system_info.contains("Processor") && system_info["Processor"].is_string()) {
        availability.processor = system_info["Processor"].get<std::string>();
    }
    if (system_info.contains("OS Version") && system_info["OS Version"].is_string()) {
        availability.os_version = system_info["OS Version"].get<std::string>();
    }

    static bool debug_printed = false;
    if (!debug_printed) {
        double max_model_size_gb = largest_mem_pool_gb > (system_ram_gb * 0.8)
                                       ? largest_mem_pool_gb
                                       : (system_ram_gb * 0.8);
        LOG(INFO, "ModelManager") << "Backend availability:" << std::endl;
        LOG(INFO, "ModelManager") << "  - NPU hardware: " << (npu_available ? "Yes" : "No")
                                  << std::endl;
        if (system_ram_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - System RAM: " << std::fixed << std::setprecision(1)
                                      << system_ram_gb << " GB (max model size: "
                                      << max_model_size_gb << " GB)" << std::endl;
        }
        if (largest_mem_pool_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - Largest memory pool: " << std::fixed
                                      << std::setprecision(1) << largest_mem_pool_gb << std::endl;
        }
        if (system_info.contains("devices") && system_info["devices"].contains("nvidia_gpu")) {
            const auto& nvidia_gpus = system_info["devices"]["nvidia_gpu"];
            if (nvidia_gpus.is_array()) {
                for (const auto& gpu : nvidia_gpus) {
                    if (gpu.value("available", false)) {
                        std::string gpu_name = gpu.value("name", "unknown");
                        std::string family = gpu.value("family", "");
                        std::string cc = gpu.value("compute_capability", "");
                        std::string suffix;
                        if (!cc.empty())
                            suffix = " (compute " + cc + ", " +
                                     (family.empty() ? "unsupported arch" : family) + ")";
                        else if (!family.empty())
                            suffix = " (" + family + ")";
                        else
                            suffix = " (arch unknown -- nvidia-smi may have failed)";
                        LOG(INFO, "ModelManager") << "  - NVIDIA GPU: " << gpu_name << suffix
                                                  << std::endl;
                    } else if (gpu.contains("error")) {
                        LOG(INFO, "ModelManager")
                            << "  - NVIDIA GPU: detection error: "
                            << gpu["error"].get<std::string>() << std::endl;
                    }
                }
                if (system_info["devices"].contains("nvidia_gpu_error")) {
                    LOG(INFO, "ModelManager")
                        << "  - NVIDIA GPU: detection error: "
                        << system_info["devices"]["nvidia_gpu_error"].get<std::string>()
                        << std::endl;
                }
            }
        }
        debug_printed = true;
    }

    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        if (availability.recipes.count(recipe)) {
            continue;
        }

        RecipeAvailability recipe_availability;
        recipe_availability.unsupported_reason = SystemInfo::check_recipe_supported(recipe);
        if (recipe_availability.unsupported_reason.empty()) {
            recipe_availability.guest_backend_missing_reason =
                SystemInfo::check_guest_backend_installed(recipe, system_info);
        }
        if (const auto* desc = backends::descriptor_for(recipe)) {
            recipe_availability.streams_model_from_storage = desc->streams_model_from_storage;
        }
        availability.recipes.emplace(recipe, std::move(recipe_availability));
    }

    return availability;
}

std::map<std::string, ModelInfo> ModelManager::filter_models_by_backend(
    const std::map<std::string, ModelInfo>& models,
    bool track_recipe_availability) {
    FilterResult result = models::filter_hardware(models, probe_backend_availability(models));

    std::map<std::string, ModelInfo> visible;
    visible.swap(result.visible);

    auto reasons = std::make_shared<FilterReasons>();
    reasons->rejected = std::move(result.rejected);
    reasons->empty_recipes = std::move(result.empty_recipes);

    // Only the full-registry pass may commit the recipe availability table;
    // incremental single-model passes would otherwise reduce it to one model.
    if (!track_recipe_availability && hardware_filter_) {
        reasons->empty_recipes = hardware_filter_->empty_recipes;
    }
    hardware_filter_ = std::const_pointer_cast<const FilterReasons>(reasons);

    return visible;
}

std::set<std::string> ModelManager::recipes_all_models_filtered_snapshot() const {
    auto snapshot = std::atomic_load(&store_snapshot_);
    const std::vector<std::string> hidden = snapshot->empty_recipes();
    return std::set<std::string>(hidden.begin(), hidden.end());
}

void ModelManager::set_cloud_registry(CloudProviderRegistry* registry) {
    cloud_registry_ = registry;
}

size_t ModelManager::refresh_cloud_models(const std::string& provider) {
    if (provider.empty() || cloud_registry_ == nullptr) {
        return 0;
    }

    // The same listing the catalogs enumerate, and it never throws: no key,
    // plaintext policy or an upstream failure all mean "nothing listed", which
    // reseeds to none and reports 0 present. Runs outside the cache lock because
    // a slow provider can take 15 s.
    const std::vector<ModelInfo> models =
        backends::CloudServer::discover_provider_models(*cloud_registry_, provider);

    StoreWrite store_write(*this);

    // Reseed: drop this provider's previously-registered entries before
    // inserting the fresh list, so a model the provider stopped exposing
    // disappears. Other providers' entries are untouched.
    for (auto it = models_cache_.begin(); it != models_cache_.end();) {
        if (it->second.recipe == "cloud" && it->second.cloud_provider == provider) {
            it = models_cache_.erase(it);
        } else {
            ++it;
        }
    }

    size_t added = 0;
    for (const auto& m : models) {
        if (m.recipe != "cloud" || m.model_name.empty()) continue;
        // Match build_cache()'s precedence exactly: emplace, don't overwrite.
        // Any pre-existing entry under the same bare cache key wins - whether
        // it's an FLM model, another cloud provider that discovered first, or
        // a builtin/extra/user record. This provider's previously-registered
        // entries are already cleared above, so emplace here is symmetric
        // with build_cache and immune to a fast /cloud/auth racing past an
        // already-populated cache.
        ModelInfo info = m;
        // discover_models() populates name/checkpoint/labels/context/cost but
        // not recipe_options; Router needs it to construct CloudServer.
        info.recipe_options = RecipeOptions("cloud", json::object());
        auto [it, inserted] = models_cache_.emplace(info.model_name, std::move(info));
        if (!inserted) {
            LOG(INFO, "ModelManager")
                << "Cloud discovery for '" << provider << "' skipping '"
                << it->first << "': name already held (recipe="
                << it->second.recipe << ")" << std::endl;
            continue;
        }
        ++added;
    }

    rebuild_public_model_aliases_locked();

    LOG(INFO, "ModelManager") << "Refreshed cloud models for provider '"
                               << provider << "': " << added << " model(s)"
                               << std::endl;
    return added;
}

size_t ModelManager::evict_cloud_models(const std::string& provider) {
    if (provider.empty()) return 0;
    StoreWrite store_write(*this);
    size_t removed = 0;
    for (auto it = models_cache_.begin(); it != models_cache_.end();) {
        if (it->second.recipe == "cloud" && it->second.cloud_provider == provider) {
            it = models_cache_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        rebuild_public_model_aliases_locked();
        LOG(DEBUG, "ModelManager") << "Evicted " << removed
                                    << " cloud model(s) for provider '"
                                    << provider << "'" << std::endl;
    }
    return removed;
}

size_t ModelManager::count_cloud_models(const std::string& provider) const {
    auto snapshot = std::atomic_load(&store_snapshot_);
    return std::count_if(snapshot->models.begin(), snapshot->models.end(),
                         [&](const auto& kv) {
                             return kv.second.recipe == "cloud" &&
                                    kv.second.cloud_provider == provider;
                         });
}

// The label set a user or inline model definition normalizes to. Kept in one
// place so model registration (register_user_model) and collection.router
// capability validation (validate_collection_request) derive the same type for
// the same definition: explicit labels + the capability booleans + the recipe's
// default deployment mode. `illegal`, when given, receives why the definition
// cannot describe a model, so the caller can refuse it in those words. The
// returned set is meaningless when it is non-empty.
static std::set<std::string> normalized_definition_labels(
    const json& model_data, std::string* illegal = nullptr) {
    const std::string recipe = model_data.value("recipe", std::string());
    std::vector<std::string> labels = {"custom"};
    for (const auto& label : model_data.value("labels", std::vector<std::string>{})) {
        add_label_once(labels, label);
    }
    if (model_data.value("reasoning", false)) add_label_once(labels, "reasoning");
    if (model_data.value("vision", false)) add_label_once(labels, "vision");
    if (model_data.value("embedding", false)) add_label_once(labels, "embeddings");
    if (model_data.value("reranking", false)) add_label_once(labels, "reranking");

    if (illegal != nullptr) {
        *illegal = lemon::backends::illegal_deployment_labels(labels, recipe);
    }
    lemon::backends::ensure_deployment_label(labels, recipe);
    return std::set<std::string>(labels.begin(), labels.end());
}

// Whether the persisted user-model entry under `key` is a router collection.
// Both register (checking the entry being overwritten) and unregister (checking
// the entry being removed) must decide whether a routing policy is disappearing,
// so the recipe lookup lives in one place.
static bool user_entry_is_router_collection(const json& user_models,
                                            const std::string& key) {
    if (!user_models.is_object() || !user_models.contains(key)) {
        return false;
    }
    return is_router_collection_recipe(
        user_models.at(key).value("recipe", std::string()));
}

void ModelManager::register_user_model(const std::string& model_name,
                                      const json& model_data,
                                      const std::string& source) {
    const std::string recipe = model_data.value("recipe", std::string());

    // Remove "user." prefix if present
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    // Filter only known, user-definable model props
    json model_entry;
    for (const std::string& prop : USER_DEFINED_MODEL_PROPS) {
        if (model_data.contains(prop)) {
            model_entry[prop] = model_data[prop];
        }
    }
    // Every registration path funnels through here — direct registration, an
    // imported collection's inline components, re-registration — so this is the
    // one gate that refuses an illegal set of mode labels. Loading an
    // already-persisted entry does not come through here; it is checked again on
    // load, where an entry written by an older version is skipped rather than
    // blocking startup.
    std::string illegal;
    std::set<std::string> labels = normalized_definition_labels(model_data, &illegal);
    if (!illegal.empty()) {
        throw InvalidModelDefinitionError(describe_illegal_labels(model_name, illegal));
    }

    model_entry["labels"] = labels;
    model_entry["suggested"] = true; // Always set suggested=true for user models

    if (!source.empty()) {
        model_entry["source"] = source;
    }

    // Single source of truth for registry-backed collections: the component list lives
    // in the cached remote-registry manifest, so the registry entry stores only the
    // repo pointer (checkpoint). Persisting `components` here would let a stale
    // local copy shadow a refreshed manifest. A pure inline collection has no
    // checkpoint pointer and keeps its authored components.
    if (is_model_collection_recipe(recipe)) {
        std::string pointer = model_entry.value("checkpoint", std::string());
        if (pointer.empty() && model_entry.contains("checkpoints") &&
            model_entry["checkpoints"].is_object()) {
            pointer = model_entry["checkpoints"].value("main", std::string());
        }
        if (!pointer.empty()) {
            model_entry.erase("components");
        }
    }

    // update_user re-reads the file, so a concurrent pull's entry survives this
    // one — a lost registration is a hard "Model not found" on the next
    // auto-load. The store write keeps save and invalidation one step.
    bool overwrote_router_collection = false;
    {
        StoreWrite store_write(*this);
        registries_.update_user([&](json& user_models) {
            overwrote_router_collection =
                user_entry_is_router_collection(user_models, clean_name);
            user_models[clean_name] = model_entry;
            return true;
        });
        cache_valid_ = false;
    }

    // A router collection carries a routing policy, so its lifecycle affects the
    // routing-helper working set. Notify when the new entry is a router
    // collection, but also when a router collection is being *replaced* by a
    // non-router recipe — otherwise the old policy silently disappears without a
    // reconcile. Registering ordinary models (e.g. a collection's helper
    // components) still must not trigger one.
    if (is_router_collection_recipe(recipe) || overwrote_router_collection) {
        notify_models_changed();
    }
}

void ModelManager::unregister_user_model(const std::string& model_name) {
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    bool was_router_collection = false;
    {
        StoreWrite store_write(*this);
        const bool wrote = registries_.update_user([&](json& user_models) {
            if (!user_models.contains(clean_name)) {
                return false;
            }
            was_router_collection =
                user_entry_is_router_collection(user_models, clean_name);
            user_models.erase(clean_name);
            return true;
        });
        if (!wrote) {
            return;
        }
        cache_valid_ = false;
    }

    if (was_router_collection) {
        notify_models_changed();
    }
}




bool ModelManager::is_model_downloaded(const std::string& model_name) {
    // Build cache if needed
    build_cache();
    // O(1) lookup - download status is in cache
    auto snapshot = std::atomic_load(&store_snapshot_);
    std::string canonical_name = snapshot->canonical_for(model_name);
    if (canonical_name.empty()) {
        canonical_name = model_name;
    }
    const ModelInfo* cached = snapshot->find(canonical_name);
    if (cached == nullptr) {
        return false;
    }
    if (!cached->downloaded ||
        download_policy(*cached) == backends::DownloadPolicy::SelfManaged ||
        artifacts_->complete(*cached)) {
        return cached->downloaded;
    }

    // Files went missing since the cache was built. Nothing rebuilds on its own,
    // so record it through a store write for every other reader.
    StoreWrite store_write(*this);
    auto it = models_cache_.find(canonical_name);
    if (it != models_cache_.end()) {
        it->second.downloaded = false;
    }
    return false;
}

backends::DownloadPolicy ModelManager::download_policy(const ModelInfo& info) const {
    return artifacts_->download_policy(info);
}

bool ModelManager::checkpoints_complete(const ModelInfo& info) const {
    return artifacts_->complete(info);
}

void ModelManager::download_registered_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress_callback) {
    // Serialize downloads per checkpoint repo. A second request for the same
    // repo (e.g. a client that timed out and retried /pull while the first
    // download is still running) must wait for the in-flight download instead
    // of writing the same .partial files concurrently, which corrupts them and
    // sends the hash verification into an endless retry-from-scratch loop.
    std::shared_ptr<std::mutex> repo_lock;
    {
        std::lock_guard<std::mutex> guard(download_locks_mutex_);
        auto& slot = download_locks_[
            models::registry_source_of(info) + ":" +
            repo_id_from_checkpoint(info.checkpoint("main"))];
        if (!slot) slot = std::make_shared<std::mutex>();
        repo_lock = slot;
    }
    std::lock_guard<std::mutex> download_lock(*repo_lock);

    // The backend's ops own the download (shared registry engine by default; flm pulls
    // via the flm CLI; cloud is a no-op).
    backends::BackendOpsContext octx;
    octx.fetch = this;
    octx.artifacts = artifacts_.get();
    backends::ops_for(info.recipe)->download_model(info, do_not_upgrade, progress_callback, octx);

    // Update cache after successful download
    update_model_in_cache(info.model_name, true);

    std::string canonical_model_name = resolve_model_name(info.model_name);
    if (is_user_model_name(canonical_model_name))
    {
        StoreWrite store_write(*this);
        auto it = models_cache_.find(canonical_model_name);
        if (it != models_cache_.end())
        {
            const bool wrote = registries_.update_user([&](json& user_models) {
                auto model = user_models.find(strip_user_model_prefix(canonical_model_name));
                if (model == user_models.end()) {
                    return false;
                }
                (*model)["size"] = it->second.size;
                return true;
            });
            if (wrote) {
                cache_valid_ = false;
            }
        }
    }
}

// Build a ModelInfo from a raw model definition (server_models.json /
// collection-manifest shape) using the same parsing as build_cache, so a
// manifest component can be compared field-for-field against a registered model.
static ModelInfo model_info_from_def(const json& def) {
    ModelInfo info;
    models::parse_checkpoints(info, def);
    info.recipe = JsonUtils::get_or_default<std::string>(def, "recipe", "");
    models::parse_model_source_fields(info, def);
    return info;
}

// Return a human-readable description of load-bearing differences between a
// registered model and a manifest's inline definition of the same component
// (empty string when they agree). Reuses model_info_from_def so identical
// definitions never produce false-positive drift.
static std::string collection_component_drift(const ModelInfo& local, const json& manifest_comp) {
    ModelInfo m = model_info_from_def(manifest_comp);
    std::vector<std::string> diffs;
    if (!m.recipe.empty() && m.recipe != local.recipe) {
        diffs.push_back("recipe (manifest='" + m.recipe + "' local='" + local.recipe + "')");
    }
    if (models::registry_source_of(m) != models::registry_source_of(local)) {
        diffs.push_back("source (manifest='" + models::registry_source_of(m) +
                        "' local='" + models::registry_source_of(local) + "')");
    }
    for (const auto& [type, ck] : m.checkpoints) {
        if (ck.empty()) continue;
        auto it = local.checkpoints.find(type);
        std::string lck = (it == local.checkpoints.end()) ? "" : it->second;
        if (ck != lck) {
            diffs.push_back("checkpoint[" + type + "] (manifest='" + ck + "' local='" + lck + "')");
        }
    }
    std::string out;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (i) out += "; ";
        out += diffs[i];
    }
    return out;
}

// Locate a collection manifest in a cached registry snapshot: any *.json file whose
// content is an object with recipe == "collection.omni". Note the two halves use
// different discovery rules by design: the *initial* remote fetch is
// filename-keyed on <RepoName>.json (see fetch_pull_variants in hf_variants.cpp),
// but once the snapshot is cached this reader is content-based, so the filename is
// not load-bearing here. Returns an empty json when no manifest is cached.
static json read_cached_collection_manifest(const models::ModelArtifactStore& artifacts,
                                           const fs::path& cache_dir) {
    fs::path snap = artifacts.active_snapshot_path(cache_dir);
    if (snap.empty()) return json();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(snap, safe_dir_options, ec)) {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec)) continue;
        if (entry.path().extension() != ".json") continue;
        json candidate;
        try {
            candidate = JsonUtils::load_from_file(path_to_utf8(entry.path()));
        } catch (const std::exception&) {
            continue;
        }
        if (candidate.is_object() &&
            is_model_collection_recipe(JsonUtils::get_or_default<std::string>(candidate, "recipe", ""))) {
            return candidate;
        }
    }
    return json();
}

// Bare component name: collection files reference components by public name;
// definitions may carry a `user.` prefix from the export transform.
static std::string bare_component_name(const std::string& name) {
    return is_user_model_name(name) ? strip_user_model_prefix(name) : name;
}

// A collection component's inline definition must be a usable model
// registration before we register it: a non-empty recipe plus at least one
// non-empty checkpoint. Fails an inline import closed instead of persisting a
// half-defined component that only blows up later during download.
static bool collection_component_def_is_valid(const json& def) {
    if (!def.is_object() || !def.contains("recipe") || !def["recipe"].is_string()) {
        return false;
    }
    if (def["recipe"].get<std::string>().empty()) {
        return false;
    }
    if (def.contains("checkpoint") && def["checkpoint"].is_string() &&
        !def["checkpoint"].get<std::string>().empty()) {
        return true;
    }
    if (def.contains("checkpoints") && def["checkpoints"].is_object()) {
        for (const auto& [_, value] : def["checkpoints"].items()) {
            if (value.is_string() && !value.get<std::string>().empty()) {
                return true;
            }
        }
    }
    return false;
}

json ModelManager::fetch_collection_manifest(const std::string& repo_id,
                                             const std::string& registry_source,
                                             bool do_not_upgrade) {
    const std::string source = remote_registry_source_name(parse_remote_registry_source(registry_source));
    fs::path cache_dir = path_from_utf8(get_hf_cache_dir()) / repo_id_to_cache_dir_name(repo_id, source);

    // A usable manifest needs both arrays; an incomplete cached copy (e.g. a
    // stale old-format file) must not satisfy do_not_upgrade - it should
    // trigger a refresh instead.
    auto manifest_complete = [](const json& m) {
        return m.is_object() &&
               m.contains("components") && m["components"].is_array() &&
               !m["components"].empty() &&
               m.contains("models") && m["models"].is_array();
    };

    json manifest = read_cached_collection_manifest(*artifacts_, cache_dir);
    bool have_cache = manifest_complete(manifest);

    bool offline = false;
    if (auto* cfg = RuntimeConfig::global()) {
        offline = cfg->offline();
    }

    if (offline) {
        if (!have_cache) {
            throw std::runtime_error(
                "Offline mode: collection definition for '" + repo_id +
                "' is not cached locally.");
        }
    } else if (!(do_not_upgrade && have_cache)) {
        // Download the collection repo (the manifest plus its other small files)
        // into the shared registry cache. A bare repo id with no variant downloads all files.
        ModelInfo manifest_info;
        manifest_info.model_name = repo_id;
        manifest_info.checkpoints["main"] = repo_id;
        try {
            manifest_info.registry_source = source;
            download_from_registry(manifest_info, nullptr);
            manifest = read_cached_collection_manifest(*artifacts_, cache_dir);
        } catch (const std::exception& e) {
            if (!have_cache) throw;
            LOG(WARNING, "ModelManager") << "Could not refresh collection manifest for "
                << repo_id << " (" << e.what() << "); using cached copy" << std::endl;
        }
    }

    if (!manifest_complete(manifest)) {
        throw std::runtime_error(
            "Collection manifest for '" + repo_id +
            "' must contain a non-empty 'components' array and a 'models' array.");
    }
    return manifest;
}

std::vector<std::string> ModelManager::register_components(const json& component_names,
                                                           const json& component_defs,
                                                           const std::string& registry_source) {
    auto def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };

    // Two passes so register_components is atomic: validate every unknown
    // component's inline definition first, and only register them once the whole
    // list is known good. A half-defined component therefore never persists a
    // stray user.* entry that a later failure would have to roll back.
    std::vector<std::string> components;
    std::vector<std::pair<std::string, json>> pending_registrations;  // canonical, def
    for (const auto& entry : component_names) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        if (name.empty()) {
            LOG(WARNING, "ModelManager") << "Skipping collection component with an "
                << "empty name" << std::endl;
            continue;
        }

        // Find the inline definition matching this component, if any.
        json def;
        if (component_defs.is_array()) {
            for (const auto& candidate : component_defs) {
                if (def_name(candidate) == name) {
                    def = candidate;
                    break;
                }
            }
        }

        // A remote collection defines one provenance domain. Components without
        // an explicit source inherit it before drift comparison or registration.
        if (def.is_object() && !def.contains("source") && !def.contains("registry_source")) {
            def["source"] = remote_registry_source_name(
                parse_remote_registry_source(registry_source));
        }

        // Components must be regular models, not collections (backstop for the
        // remote-manifest path, which does not go through validate_collection_request).
        bool def_is_collection =
            def.is_object() && is_model_collection_recipe(def.value("recipe", std::string()));
        if (def_is_collection ||
            (model_exists(name) && is_model_collection_recipe(get_model_info(name).recipe))) {
            throw std::runtime_error(
                "Collection components must be regular models, not collections: '" + name + "'");
        }

        if (model_exists(name)) {
            // Local-wins: the shipped/registered definition is authoritative. Warn
            // (do not overwrite) when the inline definition has drifted.
            if (def.is_object()) {
                ModelInfo local = get_model_info(name);
                std::string drift = collection_component_drift(local, def);
                if (!drift.empty()) {
                    LOG(WARNING, "ModelManager") << "Collection component '" << name
                        << "' differs from the local model definition; using the local "
                        << "definition. Drift: " << drift << std::endl;
                }
            }
            components.push_back(resolve_model_name(name));
        } else if (def.is_object()) {
            // Unknown component: register it from the inline definition so the
            // collection file is self-contained. Persists to user_models.json.
            std::string canonical = "user." + name;
            // Manifest content is remote input - apply the same reserved-name
            // check as the CLI and /pull registration paths.
            if (is_reserved_registration_name(canonical)) {
                LOG(WARNING, "ModelManager") << "Skipping collection component with "
                    << "reserved name: " << name << std::endl;
                continue;
            }
            // Fail closed on a half-defined component (missing recipe/checkpoint)
            // rather than registering it and failing later mid-download. The
            // throw aborts before any registration in the second pass runs.
            if (!collection_component_def_is_valid(def)) {
                throw std::runtime_error(
                    "Collection component '" + name + "' has an incomplete inline "
                    "definition (a recipe and at least one checkpoint are required).");
            }
            pending_registrations.emplace_back(canonical, def);
            components.push_back(canonical);
        } else {
            LOG(WARNING, "ModelManager") << "Skipping unknown collection component with no "
                << "inline definition: " << name << std::endl;
        }
    }

    for (const auto& [canonical, def] : pending_registrations) {
        LOG(INFO, "ModelManager") << "Registering collection component as user model: "
            << canonical << std::endl;
        register_user_model(canonical, def);
    }
    return components;
}

std::vector<std::string> ModelManager::resolve_collection_components_from_manifest(
        const std::string& repo_id, const std::string& registry_source,
        bool do_not_upgrade) {
    json manifest = fetch_collection_manifest(repo_id, registry_source, do_not_upgrade);
    return register_components(manifest["components"], manifest["models"], registry_source);
}

void ModelManager::populate_collection_components_from_cache(ModelInfo& info) {
    std::string repo_id = info.checkpoint();
    if (repo_id.empty()) return;

    fs::path cache_dir = path_from_utf8(get_hf_cache_dir()) /
        repo_id_to_cache_dir_name(repo_id, models::registry_source_of(info));
    json manifest = read_cached_collection_manifest(*artifacts_, cache_dir);
    if (!manifest.is_object() || !manifest.contains("components") ||
        !manifest["components"].is_array()) {
        return;
    }

    const models::RegistryDocuments::Pin registries = registries_.pin();

    for (const auto& entry : manifest["components"]) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        std::string canonical;
        if (registries.contains(models::RegistryDocument::Builtin, name)) {
            canonical = name;
        } else if (registries.contains(models::RegistryDocument::User, name)) {
            canonical = "user." + name;
        } else {
            canonical = name;  // not yet registered → resolves as not-downloaded
        }
        info.components.push_back(canonical);
    }

    // Surface the collection's total download size when the slim registry entry
    // omits it.
    if (info.size == 0.0 && manifest.contains("size") && manifest["size"].is_number()) {
        info.size = manifest["size"].get<double>();
    }

    // Same lift-from-manifest pattern for the optional per-collection system
    // prompt: the registry entry wins when it sets one, otherwise the published
    // remote manifest acts as the source of truth.
    if (info.system_prompt.empty() && manifest.contains("system_prompt") &&
        manifest["system_prompt"].is_string()) {
        info.system_prompt = manifest["system_prompt"].get<std::string>();
    }
}

void ModelManager::register_model(const std::string& model_name,
                                 const json& model_data,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing) {
    std::set<std::string> visited;
    download_model(model_name, model_data, true, nullptr, visited,
                   true, allow_missing_checkpoint, replace_existing);
}

void ModelManager::download_model(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback) {
    if (download_model_override_) {
        download_model_override_(model_name, model_data, do_not_upgrade, progress_callback);
        return;
    }
    std::set<std::string> visited;
    download_model(model_name, model_data, do_not_upgrade, progress_callback, visited,
                   false, false, false);
}

void ModelManager::download_model(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 std::set<std::string>& visited,
                                 bool register_only,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing) {
    // Keep a mutable registration payload so legacy re-pulls that omit the
    // registry retain the source recorded on the existing model. The original
    // request remains untouched for validation and download semantics.
    json registration_data = model_data;
    std::string actual_checkpoint;

    if (model_data.contains("checkpoints")) {
        json checkpoints = model_data["checkpoints"];
        if (!checkpoints.is_object() || !checkpoints.contains("main")) {
            throw std::runtime_error("If present, the `checkpoints` property must be an object and must contain `main`");
        }

        actual_checkpoint = checkpoints.value("main", "");
    } else {
        actual_checkpoint = model_data.value("checkpoint", "");
    }

    std::string actual_recipe = model_data.value("recipe", "");

    // If checkpoint or recipe are provided, this is a model registration
    // and the model name must have the "user." prefix
    if (!actual_checkpoint.empty() || !actual_recipe.empty()) {
        if (!is_user_model_name(model_name)) {
            throw std::runtime_error(
                "When providing 'checkpoint' or 'recipe', the model name must include the "
                "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }
    }

    // Check if model exists in registry
    bool model_registered = model_exists(model_name);

    if (!model_registered) {
        // First, check if the model exists but was filtered out (unsupported recipe)
        if (model_exists_unfiltered(model_name)) {
            // Model exists in registry but is not available on this system
            std::string filter_reason = get_model_filter_reason(model_name);
            throw std::runtime_error(
                "Model '" + model_name + "' is not available on this system. " +
                filter_reason
            );
        }

        // Model not in registry - this must be a user model registration
        // Validate it has the "user." prefix
        if (!is_user_model_name(model_name)) {
            // See UnknownModelError contract in include/lemon/model_manager.h.
            throw UnknownModelError(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }

        if (is_model_collection_recipe(actual_recipe)) {
            if (auto err = validate_collection_request(model_name, model_data)) {
                throw std::runtime_error(*err);
            }
            LOG(INFO, "ModelManager") << "Registering new collection: " << model_name << std::endl;
        } else {
            if (actual_recipe.empty()) {
                throw std::runtime_error(
                    "Model " + model_name + " is not registered with Lemonade Server. "
                    "To register it, provide the `recipe` argument."
                );
            }
            if (actual_checkpoint.empty() && !allow_missing_checkpoint) {
                throw std::runtime_error(
                    "Model " + model_name + " is not registered with Lemonade Server. "
                    "To register and install it, provide the `checkpoint` and `recipe` "
                    "arguments."
                );
            }

            // Backend-specific checkpoint validation (llamacpp: GGUF needs :variant).
            if (!actual_checkpoint.empty()) {
                if (auto err = backends::ops_for(actual_recipe)->validate_registration_checkpoint(
                        actual_checkpoint);
                    !err.empty()) {
                    throw std::runtime_error(err);
                }
            }

            LOG(INFO, "ModelManager") << "Registering new user model: " << model_name << std::endl;
        }
    } else {
        // Model is registered - if checkpoint not provided, look up from registry.
        // If checkpoint/recipe are provided, allow an idempotent re-pull of the
        // same registration but never silently replace a user model with a
        // different remote checkpoint or registry. Silent replacement is what
        // made a previously installed GGUF variant disappear when another variant
        // reused the same generated user.* name.
        auto info = get_model_info(model_name);
        if (!registration_data.contains("source") &&
            !registration_data.contains("registry_source")) {
            registration_data["source"] = models::registry_source_of(info);
        }

        const bool explicit_definition =
            !actual_recipe.empty() || !actual_checkpoint.empty() ||
            model_data.contains("checkpoints") || model_data.contains("components");
        if (register_only && replace_existing &&
            is_user_model_name(model_name) && explicit_definition) {
            if (is_model_collection_recipe(actual_recipe)) {
                if (auto err = validate_collection_request(model_name, model_data)) {
                    throw std::runtime_error(*err);
                }
            }
            model_registered = false;
            LOG(INFO, "ModelManager") << "Replacing user model definition: "
                                      << model_name << std::endl;
        } else {
            bool is_collection_overwrite = is_model_collection_recipe(actual_recipe) &&
                                            model_data.contains("components");
            if (is_collection_overwrite) {
                // Validate the original user-authored request, not registration_data:
                // the latter is enriched with the persisted registry source, which is
                // not part of the public routing-policy document the parser accepts.
                if (auto err = validate_collection_request(model_name, model_data)) {
                    throw std::runtime_error(*err);
                }
                model_registered = false;
                LOG(INFO, "ModelManager") << "Overwriting collection: "
                                          << model_name << std::endl;
            } else if (actual_checkpoint.empty()) {
                actual_checkpoint = info.checkpoint();
                actual_recipe = info.recipe;
            } else {
                std::string conflict = describe_registration_conflict(info, registration_data);
                if (!conflict.empty()) {
                    throw std::runtime_error(
                        "Model '" + model_name + "' is already registered with different "
                        "model metadata: " + conflict + ". Choose a different model name "
                        "for this registry checkpoint."
                    );
                }
                if (actual_recipe.empty()) {
                    actual_recipe = info.recipe;
                } else {
                    model_registered = false;
                }
            }
        }
    }

    // Parse checkpoint
    std::string repo_id = actual_checkpoint;
    std::string variant = "";

    size_t colon_pos = actual_checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = actual_checkpoint.substr(0, colon_pos);
        variant = actual_checkpoint.substr(colon_pos + 1);
    }

    // Register collections early - the fan-out below calls get_model_info().
    // Track that this call created the registration so component-resolution
    // failures can roll it back instead of leaving a broken entry behind.
    bool collection_registered_this_call = false;
    if (is_model_collection_recipe(actual_recipe) && is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
        model_registered = true;
        collection_registered_this_call = true;
    }

    if (register_only && is_model_collection_recipe(actual_recipe)) {
        return;
    }

    // Collections don't have their own backend - download each component instead.
    //
    // Persistence follows one rule, uniform across models and collections: a
    // registry entry stores what was *authored locally*; anything *fetched from
    // a remote registry* lives in the shared model-hub cache and is rebuilt on lookup. A regular model
    // persists its recipe/checkpoint but not its weights; an inline collection
    // persists its components (authored); a registry-backed collection persists only
    // its `checkpoint` pointer, with the component list rebuilt from the cached
    // manifest (fetched). The one exception: a fetched manifest registers its
    // components as user models so they're routable.
    if (is_model_collection_recipe(actual_recipe)) {
        // Cycle guard: re-entering the same collection on the current call
        // chain means the user registered a circular reference (e.g. user.A
        // includes user.B which includes user.A). Without this, the fan-out
        // would recurse until the stack or HTTP timeout gives out.
        if (!visited.insert(model_name).second) {
            throw std::runtime_error(
                "Cycle detected in collection components: '" + model_name +
                "' transitively references itself. Edit the offending collection "
                "in user_models.json to remove the cycle."
            );
        }

        auto info = get_model_info(model_name);
        std::vector<std::string> components = info.components;

        try {
            if (model_data.contains("models") && model_data["models"].is_array() &&
                model_data.contains("components") && model_data["components"].is_array()) {
                // Collection file import: the body carries each component's definition
                // inline in `models` (the exported-collection format). Register unknown
                // components from those definitions and canonicalize the list.
                components = register_components(model_data["components"], model_data["models"],
                                                models::registry_source_of(info));

                // The early registration above persisted the raw component names from
                // the file; re-register with the canonical list so cache lookups
                // (check_component_downloaded, update_model_in_cache) match after a
                // rebuild. register_user_model drops the bulky `models` array and,
                // for a registry-backed collection (one with a checkpoint pointer), also
                // drops `components` so the cached manifest stays the sole source of
                // truth - only a pure inline collection persists its component list.
                if (is_user_model_name(model_name) && !components.empty()) {
                    json reg = registration_data;
                    reg["components"] = components;
                    register_user_model(model_name, reg);
                }
            } else if (!repo_id.empty() && (components.empty() || !do_not_upgrade)) {
                // Registry-backed collections keep their full definition — the component list
                // and each component's model object — in the configured registry as an exported
                // collection JSON. A non-empty checkpoint marks such a collection; fetch
                // the manifest to learn its components. On an explicit pull
                // (do_not_upgrade=false) always refresh so newly added components are
                // picked up.
                components = resolve_collection_components_from_manifest(
                    repo_id, models::registry_source_of(info), do_not_upgrade);
            }

            if (components.empty()) {
                throw std::runtime_error("Collection '" + model_name + "' has no components defined");
            }
        } catch (...) {
            // Roll back the early registration when component resolution fails,
            // so a failed import does not persist a collection entry whose
            // components were never resolved. Downloads below are not rolled
            // back - a mid-download failure leaves a valid, re-pullable entry.
            if (collection_registered_this_call) {
                LOG(WARNING, "ModelManager") << "Component resolution failed; unregistering "
                    << "collection: " << model_name << std::endl;
                unregister_user_model(model_name);
            }
            throw;
        }
        LOG(INFO, "ModelManager") << "Downloading " << components.size()
                                  << " component(s) for collection: " << model_name << std::endl;

        // Wrap the callback so recursive per-component downloads don't each
        // emit a "complete" event - the SSE stream should only see one final
        // completion after every component finishes.
        //
        // Capture progress_callback by value (not by reference) so `forward`
        // owns a copy of the std::function. This keeps the callback usable
        // even if `forward` is ever copied or outlives this stack frame;
        // today it's only consumed synchronously below, but the defensive
        // copy is free (std::function is copyable) and removes a latent
        // dangling-reference hazard if the recursion pattern changes.
        DownloadProgressCallback forward = nullptr;
        if (progress_callback) {
            forward = [progress_callback](const DownloadProgress& p) -> bool {
                if (p.complete) return true;
                return progress_callback(p);
            };
        }

        for (const auto& component : components) {
            if (!model_exists(component)) {
                LOG(WARNING, "ModelManager") << "Skipping unknown component: " << component << std::endl;
                continue;
            }
            auto comp_info = get_model_info(component);
            if (comp_info.downloaded && do_not_upgrade) {
                LOG(INFO, "ModelManager") << "Component already downloaded: " << component << std::endl;
                continue;
            }
            LOG(INFO, "ModelManager") << "Downloading component: " << component << std::endl;
            json comp_data = json::object();
            download_model(component, comp_data, do_not_upgrade, forward, visited, false, false, false);
        }

        // A registry-backed collection's in-memory components were empty until the
        // manifest was fetched above (build_cache populated them empty at startup
        // when no manifest was cached yet). Invalidate the cache so the next lookup
        // rebuilds the collection entry from the now-cached manifest - populating
        // its components and recomputing its downloaded status - instead of leaving
        // the stale empty-components/not-downloaded state until a restart.
        if (!repo_id.empty()) {
            StoreWrite store_write(*this);
            cache_valid_ = false;
        }

        // Emit a single completion event for the whole collection
        if (progress_callback) {
            DownloadProgress progress;
            progress.complete = true;
            progress.percent = 100;
            (void)progress_callback(progress);
        }
        // Scope cycle detection to the current recursion stack: a sibling branch
        // that legitimately revisits this collection (DAG, e.g. A->{B,C}->D
        // where D is itself a collection) must not be misread as a cycle.
        visited.erase(model_name);
        return;
    }

    // Check if this recipe is supported on the current system
    bool disable_filtering = false;
    if (auto* cfg = RuntimeConfig::global()) {
        disable_filtering = cfg->disable_model_filtering();
    }
    std::string unsupported_reason = SystemInfo::check_recipe_supported(actual_recipe);
    if (!unsupported_reason.empty() && !disable_filtering) {
        throw std::runtime_error(
            "Model '" + model_name + "' cannot be used on this system (recipe: " + actual_recipe + "): " +
            unsupported_reason
        );
    }

    // Persist registration and recipe options BEFORE the cache-first shortcut
    // below. A registration/import/overwrite that targets an already-downloaded
    // model must still update user_models.json and recipe_options.json. The
    // do_not_upgrade fast return only skips the remote-registry download/update
    // check — it must never skip the metadata the caller asked us to record,
    // otherwise an import that reports success would silently leave the old
    // checkpoints/options in place.
    if (is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
    }

    auto model_info = get_model_info(model_name);

    const std::string download_source = requested_registry_source(model_data);
    if (!download_source.empty() &&
        download_policy(model_info) == backends::DownloadPolicy::SelfManaged) {
        model_info.registry_source = download_source;
    }

    if (model_data.contains("recipe_options")) {
        // Merge import recipe_options on top of the already-merged options
        // (which include image_defaults + JSON recipe_options + user-saved).
        // This preserves the full 3-level merge from add_model_to_cache while
        // layering in any import-specific overrides (e.g. sdcpp_args).
        json merged = model_info.recipe_options.to_json();
        for (auto& [key, value] : model_data["recipe_options"].items()) {
            merged[key] = value;
        }
        model_info.recipe_options = RecipeOptions(model_info.recipe, merged);
        save_model_options(model_info);
    }

    if (register_only) {
        return;
    }

    LOG(INFO, "ModelManager") << "Downloading model: " << repo_id;
    if (!variant.empty()) {
        LOG(INFO, "ModelManager") << " (variant: " << variant << ")";
    }
    LOG(INFO, "ModelManager") << std::endl;

    // Check if offline mode
    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline()) {
            LOG(INFO, "ModelManager") << "Offline mode enabled, skipping download" << std::endl;
            return;
        }
    }

    // CRITICAL: If do_not_upgrade=true AND model is already downloaded, skip the
    // remote-registry update check. Registration and recipe options were already
    // persisted above, so an import/overwrite still takes effect on disk.
    // The do_not_upgrade flag means:
    //   - Load/inference endpoints: Don't check the remote registry for updates (use cache if available)
    //   - Pull endpoint: Always check the recorded registry for the latest version (do_not_upgrade=false)
    if (do_not_upgrade && is_model_downloaded(model_name)) {
        LOG(INFO, "ModelManager") << "Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }

    std::map<std::string, std::filesystem::path> resolved_paths_before;
    {
        StoreWrite store_write(*this);
        auto it = models_cache_.find(model_info.model_name);
        if (it != models_cache_.end()) {
            for (const auto& [k, v] : it->second.resolved_paths) {
                resolved_paths_before[k] = path_from_utf8(v).lexically_normal();
            }
        }
    }

    download_registered_model(model_info, do_not_upgrade, progress_callback);

    std::map<std::string, std::filesystem::path> resolved_paths_after;
    {
        StoreWrite store_write(*this);
        auto it = models_cache_.find(model_info.model_name);
        if (it != models_cache_.end()) {
            for (const auto& [k, v] : it->second.resolved_paths) {
                resolved_paths_after[k] = path_from_utf8(v).lexically_normal();
            }
        }
    }

    if (resolved_paths_before != resolved_paths_after) {
        change_notifier_.model_updated(model_name);
    }


}



void ModelManager::download_from_manifest(const json& manifest,
                                          std::map<std::string, std::string>& headers,
                                          DownloadProgressCallback progress_callback) {
    const DownloadPlan plan = download_plan_from_manifest(manifest);
    ArtifactTransfer transfer = ArtifactTransfer::with_default_transfers();
    transfer.execute(plan, headers, progress_callback);
}

// Download model files from the configured remote registry.
// =========================================================
// This function always queries the selected registry for the current file tree.
// The caller is responsible for applying do_not_upgrade/offline policy first.
//
// The caller (download_model) is responsible for checking do_not_upgrade and
// calling is_model_downloaded() before invoking this function.
//
// Download capabilities by backend:
//   - Lemonade Router (ModelManager): downloads non-FLM registry models
//   - FLM backend: ✅ Downloads FLM models via 'flm pull' command
//   - llama-server backend: ❌ Cannot download (expects GGUF files pre-cached)
//   - ryzenai-server backend: ❌ Cannot download (expects ONNX files pre-cached)
void ModelManager::download_from_registry(const ModelInfo& info,
                                          DownloadProgressCallback progress_callback) {
    const std::string main_repo_id = repo_id_from_checkpoint(info.checkpoint("main"));
    const std::string main_variant = variant_from_checkpoint(info.checkpoint("main"));
    if (main_repo_id.empty()) {
        throw std::runtime_error("Model checkpoint does not contain a repository id");
    }

    const std::string source_name = models::registry_source_of(info);
    const auto source = parse_remote_registry_source(source_name);
    const auto& registry = model_registry(source);
    const std::string source_display = remote_registry_display_name(source);
    std::map<std::string, std::string> headers = registry.auth_headers();

    const fs::path cache_root = path_from_utf8(get_hf_cache_dir());
    ensure_create_directories(cache_root);

    LOG(INFO, "ModelManager") << "Fetching repository file list from "
                               << source_display << ": " << main_repo_id << std::endl;

    std::map<std::string, RegistryRepository> repositories;
    repositories.emplace(main_repo_id, registry.fetch_repository(main_repo_id));

    std::map<std::string, std::vector<std::string>> files_to_download;
    std::vector<std::string> main_repo_files;
    for (const auto& file : repositories.at(main_repo_id).files) {
        if (!file.directory) main_repo_files.push_back(file.path);
    }
    LOG(INFO, "ModelManager") << "Repository contains " << main_repo_files.size()
                               << " files" << std::endl;

    // The update check uses the same selection helper, so a file pull would
    // download is always a file the check compares.
    files_to_download[main_repo_id] = registry_files::select_main_repo_files(
        main_repo_id, info.recipe, main_variant, main_repo_files);

    // Auxiliary checkpoints inherit the model-level registry source. This is
    // intentional: a registration has one provenance and update domain.
    // Pull must reject a malformed auxiliary checkpoint loudly, where the
    // update check (group_aux_checkpoint_variants) silently skips it.
    for (const auto& [type, checkpoint] : info.checkpoints) {
        if (type == "main" || backends::ops_for(info.recipe)->acquires_lazily(type)) continue;
        if (repo_id_from_checkpoint(checkpoint).empty() ||
            variant_from_checkpoint(checkpoint).empty()) {
            throw std::runtime_error("Additional checkpoints must contain an exact repository variant");
        }
    }
    for (const auto& [repo_id, variants] :
         registry_files::group_aux_checkpoint_variants(info.recipe, info.checkpoints)) {
        if (!repositories.count(repo_id)) {
            repositories.emplace(repo_id, registry.fetch_repository(repo_id));
        }
        const auto& repo = repositories.at(repo_id);
        for (const auto& variant : variants) {
            const bool exists = std::any_of(repo.files.begin(), repo.files.end(),
                [&](const RegistryFile& file) { return !file.directory && file.path == variant; });
            if (!exists) {
                throw std::runtime_error("Additional checkpoint file not found on " +
                                         source_display + ": " + repo_id + ":" + variant);
            }
            files_to_download[repo_id].push_back(variant);
        }
    }

    for (auto& [repo_id, files] : files_to_download) {
        (void)repo_id;
        std::set<std::string> seen;
        std::vector<std::string> unique_files;
        for (auto& filename : files) {
            if (seen.insert(filename).second) {
                unique_files.push_back(filename);
            }
        }
        files = std::move(unique_files);
    }

    int total_files = 0;
    LOG(INFO, "ModelManager") << "Identified files to download:" << std::endl;
    for (const auto& [repo_id, files] : files_to_download) {
        for (const auto& filename : files) {
            ++total_files;
            LOG(INFO, "ModelManager") << "  - " << source_name << ':' << repo_id
                                       << ':' << filename << std::endl;
        }
    }
    if (total_files == 0) {
        throw std::runtime_error("No files selected for download from " + source_display +
                                 " repository " + main_repo_id);
    }

    std::map<std::string, fs::path> repo_cache_paths;
    std::map<std::string, std::string> repo_previous_refs;
    std::map<std::string, std::string> repo_snapshot_paths;
    std::map<std::string, std::string> repo_snapshot_ids;
    std::map<std::string, std::string> repo_download_paths;

    for (const auto& [repo_id, files] : files_to_download) {
        if (files.empty()) continue;
        const auto& repo = repositories.at(repo_id);
        fs::path repo_cache = cache_root / repo_id_to_cache_dir_name(repo_id, source_name);
        ensure_create_directories(repo_cache);
        const std::string previous_ref = artifacts_->active_snapshot_ref(repo_cache);
        fs::path snapshot = repo_cache / "snapshots" / repo.snapshot_id;
        ensure_create_directories(snapshot);

        repo_cache_paths[repo_id] = repo_cache;
        repo_previous_refs[repo_id] = previous_ref;
        repo_snapshot_paths[repo_id] = path_to_utf8(snapshot);
        repo_snapshot_ids[repo_id] = repo.snapshot_id;
        repo_download_paths[repo_id] = path_to_utf8(snapshot);
    }

    // Preserve the existing HF optimization: if every selected artifact is
    // byte-identical at the new commit, keep refs/main on the previous snapshot.
    // ModelScope snapshots are tree fingerprints and currently advance together
    // with the reported tree, because its branch API has no HF-style commit pin.
    std::set<std::string> repos_reusing_previous_snapshot;
    if (source == models::huggingface_descriptor().id) {
        for (const auto& [repo_id, files] : files_to_download) {
            if (files.empty()) continue;
            const std::string current_ref = repo_snapshot_ids.at(repo_id);
            const std::string previous_ref = repo_previous_refs.at(repo_id);
            if (previous_ref.empty() || previous_ref == current_ref) continue;

            // Union with the previous revision's listing so a file it dropped
            // isn't missed; files_to_download itself stays current-revision-only.
            // Merge, not replace: files also carries same-repo aux checkpoints.
            std::vector<std::string> comparison_files = files;
            if (repo_id == main_repo_id) {
                try {
                    const RegistryRepository previous_repo =
                        registry.fetch_repository(repo_id, previous_ref);
                    std::vector<std::string> previous_repo_files;
                    for (const auto& file : previous_repo.files) {
                        if (!file.directory) previous_repo_files.push_back(file.path);
                    }
                    comparison_files = registry_files::merge_reuse_comparison_files(
                        files, repo_id, info.recipe, main_variant, previous_repo_files,
                        main_repo_files);
                } catch (const std::exception& e) {
                    LOG(DEBUG, "ModelManager")
                        << "Could not confirm no files were removed from " << repo_id
                        << " since " << previous_ref << ", re-downloading: " << e.what()
                        << std::endl;
                    continue;
                } catch (...) {
                    LOG(DEBUG, "ModelManager")
                        << "Could not confirm no files were removed from " << repo_id
                        << " since " << previous_ref << ", re-downloading" << std::endl;
                    continue;
                }
            }

            const auto current_metadata = registry_files::fetch_file_metadata_for_ref(
                repo_id, current_ref, comparison_files, headers, nullptr);
            const auto previous_metadata = registry_files::fetch_file_metadata_for_ref(
                repo_id, previous_ref, comparison_files, headers, nullptr);
            const fs::path previous_snapshot =
                repo_cache_paths.at(repo_id) / "snapshots" / previous_ref;
            if (registry_files::can_reuse_previous_hf_snapshot(
                    repo_id, comparison_files, previous_snapshot, current_metadata,
                    previous_metadata)) {
                repo_download_paths[repo_id] = path_to_utf8(previous_snapshot);
                repos_reusing_previous_snapshot.insert(repo_id);
                LOG(INFO, "ModelManager") << "Keeping active Hugging Face snapshot for "
                    << repo_id << " at " << previous_ref
                    << " because selected artifacts are unchanged in " << current_ref
                    << std::endl;
            }
        }
    }

    json manifest;
    manifest["repo_id"] = main_repo_id;
    manifest["model_name"] = info.model_name;
    manifest["registry_source"] = source_name;
    manifest["commit_hash"] = repo_snapshot_ids.at(main_repo_id);
    manifest["download_path"] = repo_snapshot_paths.at(main_repo_id);
    manifest["files_count"] = total_files;
    manifest["files"] = json::array();

    for (const auto& [repo_id, files] : files_to_download) {
        const auto& repo = repositories.at(repo_id);
        std::map<std::string, RegistryFile> metadata;
        for (const auto& file : repo.files) metadata[file.path] = file;

        for (const auto& filename : files) {
            json entry;
            entry["name"] = filename;
            entry["url"] = registry.resolve_file_url(repo_id, repo.revision, filename);
            entry["download_path"] = repo_download_paths.at(repo_id);
            const auto it = metadata.find(filename);
            entry["size"] = it == metadata.end() ? 0 : it->second.size;
            if (it != metadata.end() && !it->second.hash.empty()) {
                entry["hash"] = {
                    {"algorithm", it->second.hash_algorithm},
                    {"value", it->second.hash}
                };
            }
            manifest["files"].push_back(std::move(entry));
        }
    }

    const fs::path manifest_path =
        path_from_utf8(repo_snapshot_paths.at(main_repo_id)) / ".download_manifest.json";
    JsonUtils::save_to_file(manifest, path_to_utf8(manifest_path));
    download_from_manifest(manifest, headers, progress_callback);
    std::error_code manifest_ec;
    fs::remove(manifest_path, manifest_ec);

    for (const auto& [repo_id, snapshot_id] : repo_snapshot_ids) {
        const fs::path& cache_path = repo_cache_paths.at(repo_id);
        const bool reusing_previous_snapshot =
            repos_reusing_previous_snapshot.count(repo_id) != 0;
        const std::string active_snapshot_id = reusing_previous_snapshot
            ? repo_previous_refs.at(repo_id)
            : snapshot_id;

        artifacts_->write_active_snapshot(cache_path, active_snapshot_id);
        if (reusing_previous_snapshot) {
            artifacts_->remove_unused_snapshot(
                cache_path, repo_snapshot_paths.at(repo_id), active_snapshot_id);
        }

        const auto& repo = repositories.at(repo_id);
        artifacts_->record_processed_model(cache_path, info, models::Provenance{
            source_name,
            repo_id,
            repo.revision,
            active_snapshot_id,
            snapshot_id,
        });
    }

    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);
    }

    LOG(INFO, "ModelManager") << "All files downloaded and validated from "
                               << source_display << std::endl;
    LOG(INFO, "ModelManager") << "Download location: "
        << repo_download_paths.at(main_repo_id) << std::endl;
}


void ModelManager::delete_model(const std::string& model_name) {
    auto info = get_model_info(model_name);
    std::string canonical_model_name = info.model_name;

    LOG(INFO, "ModelManager") << "Deleting model: " << canonical_model_name << std::endl;
    LOG(INFO, "ModelManager") << "Checkpoint: " << info.checkpoint() << std::endl;
    LOG(INFO, "ModelManager") << "Recipe: " << info.recipe << std::endl;

    // Removing a router collection drops its policy: reconcile helpers once the
    // delete actually completes (fires on normal return, not on an exception, so
    // a retryable file-lock failure doesn't reconcile against a still-present
    // policy). Ordinary models carry no policy and are skipped.
    const bool notify_on_delete = is_router_collection_recipe(info.recipe);
    const int uncaught_on_entry = std::uncaught_exceptions();
    struct DeleteNotifier {
        ModelManager* manager;
        bool enabled;
        int uncaught_on_entry;
        ~DeleteNotifier() {
            if (enabled && std::uncaught_exceptions() == uncaught_on_entry) {
                manager->notify_models_changed();
            }
        }
    } delete_notifier{this, notify_on_delete, uncaught_on_entry};

    // Handle extra models (from --extra-models-dir) - these are user-managed external files
    if (canonical_model_name.substr(0, 6) == "extra.") {
        throw std::runtime_error("Cannot delete extra models via API. Models in --extra-models-dir are user-managed. "
                                 "Delete the file directly from: " + info.checkpoint());
    }
    // Everything on disk is the artifact store's decision; this decides what the
    // caller hears and what the registry loses.
    const models::DeleteReport report = artifacts_->delete_artifacts(info, all_models());

    // Bytes outside the models directory belong to whoever placed them. A user
    // registration is the only record of such a model, so forgetting it is the
    // whole delete; a registered model is not ours to forget.
    if (report.outside_managed_cache && !is_user_model_name(canonical_model_name)) {
        throw std::runtime_error("Cannot delete a registered model outside its managed cache. "
                                 "External files are user-managed; remove them manually.");
    }
    if (report.cleaned_partial_download) {
        LOG(INFO, "ModelManager") << "Cleaned up an interrupted download of "
                                  << canonical_model_name << std::endl;
    }

    forget_registered_model(canonical_model_name);
    remove_model_from_cache(canonical_model_name);
}

std::map<std::string, ModelInfo> ModelManager::all_models() const {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    return models_cache_;
}

// Drop the model's own registration, if it has one. Rows the catalog re-creates
// have none.
void ModelManager::forget_registered_model(const std::string& canonical_model_name) {
    if (!is_user_model_name(canonical_model_name)) {
        return;
    }
    StoreWrite store_write(*this);
    registries_.update_user([&](json& user_models) {
        user_models.erase(strip_user_model_prefix(canonical_model_name));
        return true;
    });
    cache_valid_ = false;
    LOG(INFO, "ModelManager") << "\u2713 Removed from user_models.json" << std::endl;
}

json ModelManager::cleanup_orphaned_cache(bool dry_run) {
    build_cache();

    const models::CleanupReport report = artifacts_->cleanup_orphaned_cache(all_models(), dry_run);

    json orphaned_files = json::array();
    for (const auto& orphan : report.orphaned) {
        orphaned_files.push_back({
            {"path", orphan.path},
            {"size", orphan.size},
            {"model", orphan.model},
            {"type", orphan.type},
            {"belongs_to", orphan.belongs_to}
        });
    }

    LOG(INFO, "ModelManager") << "Cache cleanup: found " << report.orphaned.size()
                              << " orphaned file(s), " << (report.total_bytes / (1024 * 1024))
                              << " MB" << (dry_run ? " (dry run)" : " (deleted)") << std::endl;

    json result;
    result["orphaned_files"] = orphaned_files;
    result["total_bytes"] = report.total_bytes;
    result["dry_run"] = dry_run;
    return result;
}

ModelInfo ModelManager::get_model_info(const std::string& model_name) {
    // Build cache if needed
    build_cache();

    // O(1) lookup in the published snapshot
    auto snapshot = std::atomic_load(&store_snapshot_);
    std::string canonical_name = snapshot->canonical_for(model_name);
    if (canonical_name.empty()) {
        canonical_name = model_name;
    }
    if (const ModelInfo* found = snapshot->find(canonical_name)) {
        return *found;
    }

    throw std::runtime_error("Model not found: " + model_name);
}

std::string ModelManager::resolve_model_name(const std::string& model_name) {
    build_cache();

    auto snapshot = std::atomic_load(&store_snapshot_);
    std::string canonical_name = snapshot->canonical_for(model_name);
    return canonical_name.empty() ? model_name : canonical_name;
}

std::string ModelManager::get_public_model_name(const std::string& model_name) {
    build_cache();

    auto snapshot = std::atomic_load(&store_snapshot_);
    std::string public_name = snapshot->public_name_for(model_name);
    return public_name.empty() ? model_name : public_name;
}

bool ModelManager::model_exists(const std::string& model_name) {
    // Build cache if needed
    build_cache();

    // O(1) lookup in the published snapshot
    {
        auto snapshot = std::atomic_load(&store_snapshot_);
        std::string canonical_name = snapshot->canonical_for(model_name);
        if (canonical_name.empty()) {
            canonical_name = model_name;
        }
        if (snapshot->find(canonical_name) != nullptr) {
            return true;
        }
    }

    return false;
}

std::optional<std::string> ModelManager::validate_collection_request(
    const std::string& model_name, const json& model_data) {
    // A registry-backed collection is registered as a pointer: recipe + a checkpoint
    // that names the HF repo, with no inline components. /pull downloads the
    // repo's manifest to disk and resolves the components from it, so there is
    // nothing to validate here - accept the pointer body.
    std::string checkpoint_pointer = model_data.value("checkpoint", std::string());
    if (checkpoint_pointer.empty() && model_data.contains("checkpoints") &&
        model_data["checkpoints"].is_object()) {
        checkpoint_pointer = model_data["checkpoints"].value("main", std::string());
    }
    const bool has_components =
        model_data.contains("components") && model_data["components"].is_array() &&
        !model_data["components"].empty();
    if (!has_components) {
        if (!checkpoint_pointer.empty()) {
            return std::nullopt;  // pointer-only registry-backed collection
        }
        return std::string("Collection recipe requires a non-empty 'components' array");
    }
    // An inline collection import carries its component definitions in a `models`
    // array. Every component must be *resolvable*: either it is already
    // registered locally (local-wins) or it has a matching definition in
    // `models`. Validate this per-component and fail closed. `models` being
    // present is not a blanket license - a component it omits would otherwise
    // pass here and then be silently skipped during registration, persisting a
    // collection smaller than the imported file describes. Mirror the
    // name-matching that register_components() uses (bare names; `model_name`
    // or `id` keys) so this gate rejects exactly what registration would drop.
    const bool has_models_array =
        model_data.contains("models") && model_data["models"].is_array();
    auto inline_def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };
    auto find_inline_def = [&](const std::string& bare) -> const json* {
        if (!has_models_array) return nullptr;
        for (const auto& candidate : model_data["models"]) {
            if (inline_def_name(candidate) == bare) return &candidate;
        }
        return nullptr;
    };
    const std::string bare_collection = bare_component_name(model_name);
    for (const auto& component : model_data["components"]) {
        if (!component.is_string()) {
            return std::string("components entries must be strings");
        }
        std::string component_name = component.get<std::string>();
        std::string bare = bare_component_name(component_name);
        // Self-reference: compare bare forms so a collection `user.MyCol` with
        // `components: ["MyCol"]` is rejected too, not only the exact-string case.
        if (bare == bare_collection) {
            return "Collection cannot reference itself: " + component_name;
        }
        // Components must be regular models, not collections.
        const json* def = find_inline_def(bare);
        bool is_collection_component =
            (def != nullptr && def->is_object() &&
             is_model_collection_recipe(def->value("recipe", std::string()))) ||
            (model_exists(bare) && is_model_collection_recipe(get_model_info(bare).recipe));
        if (is_collection_component) {
            return "Collection components must be regular models, not collections: '" +
                   component_name + "'";
        }
        if (has_models_array) {
            if (!model_exists(bare) && def == nullptr) {
                return "Inline collection component '" + component_name +
                       "' has no matching definition in 'models' and is not a "
                       "registered model. Every component must be defined inline "
                       "(matching 'model_name') or reference an already-registered "
                       "model.";
            }
            // A component that resolves to its inline definition (rather than an
            // already-registered model) must be a usable registration. Reject a
            // half-defined component here instead of registering it and failing
            // later mid-download.
            if (!model_exists(bare) && def != nullptr &&
                !collection_component_def_is_valid(*def)) {
                return "Inline collection component '" + component_name +
                       "' has an incomplete definition in 'models' (a recipe and "
                       "at least one checkpoint are required).";
            }
            // register_user_model() would throw on this once the import reached
            // registration, leaving a partly-imported collection behind. Report
            // it here, where the import can still be refused whole.
            if (!model_exists(bare) && def != nullptr) {
                std::string illegal;
                normalized_definition_labels(*def, &illegal);
                if (!illegal.empty()) {
                    return describe_illegal_labels(component_name, illegal);
                }
            }
        } else if (!model_exists(component_name)) {
            return "Collection component not registered: '" + component_name +
                   "'. Pull or register it before referencing it in a collection.";
        }
    }
    if (is_router_collection_recipe(model_data.value("recipe", std::string()))) {
        // Same resolution shape as build_cache()'s, differing only in its
        // tiers: this one resolves live and falls back to the request's own
        // inline `models[]`, because an import validates names that are not
        // registered yet. Unresolvable names are kept as authored by
        // make_routing_parse_options, so component-role validation can still
        // compare them against the authored components array.
        RoutingPolicyParseOptions options = make_routing_parse_options({
            [this](const std::string& name) -> std::optional<std::string> {
                try {
                    return resolve_model_name(name);
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this](const std::string& name) -> std::optional<ModelType> {
                try {
                    return get_model_info(name).type;
                } catch (...) {
                    return std::nullopt;
                }
            },
            [this, &find_inline_def](const std::string& name) -> std::optional<ModelType> {
                // Not registered - the name may match an inline `models[]`
                // definition in this same request. Derive the type exactly as
                // register_user_model() would once that definition is
                // registered, so validation and runtime cannot disagree
                // (absent labels default to LLM, same as everywhere else).
                // nullopt only when no definition exists at all - the
                // "truly unknown" case.
                const json* def = find_inline_def(bare_component_name(name));
                if (!def) {
                    return std::nullopt;
                }
                std::set<std::string> label_set = normalized_definition_labels(*def);
                return get_model_type_from_labels(
                    std::vector<std::string>(label_set.begin(), label_set.end()));
            },
        });
        try {
            // Strip pull-protocol keys (stream, subscribe, do_not_upgrade, …)
            // that the client merges into the body but the policy parser does
            // not know about. Passing the raw request body would trigger the
            // reject_unknown_keys check inside parse_route_policy_collection.
            json policy_json = json::object();
            for (const auto& key : routing_policy_root_keys()) {
                if (model_data.contains(key)) policy_json[key] = model_data.at(key);
            }
            RoutePolicy policy = parse_route_policy_collection(policy_json, options);
            RoutingPolicyEngine(std::move(policy), ClassifierServices{});
        } catch (const std::exception& e) {
            return std::string("Invalid collection.router routing policy: ") + e.what();
        }
    }
    return std::nullopt;
}

bool ModelManager::model_exists_unfiltered(const std::string& model_name) {
    const models::RegistryDocuments::Pin registries = registries_.pin();
    auto exists_in_registries = [&](const std::string& name) -> bool {
        // Built-ins are keyed bare.
        if (registries.contains(models::RegistryDocument::Builtin, name)) {
            return true;
        }
        if (auto canon = parse_canonical_id(name)) {
            switch (canon->source) {
                case ModelSource::Registered:
                    return registries.contains(models::RegistryDocument::User,
                                               canon->bare_name);
                case ModelSource::Builtin:
                    return registries.contains(models::RegistryDocument::Builtin,
                                               canon->bare_name);
                case ModelSource::Imported:
                    // extra.* models are filesystem-discovered; not in either JSON registry.
                    return false;
            }
        }
        return false;
    };

    if (exists_in_registries(model_name)) {
        return true;
    }

    std::string canonical_name = resolve_model_name(model_name);
    if (exists_in_registries(canonical_name)) {
        return true;
    }

    return false;
}

ModelInfo ModelManager::get_model_info_unfiltered(const std::string& model_name) {
    ModelInfo info;
    std::string registry_name = model_name;
    bool is_user_lookup = false;

    const models::RegistryDocuments::Pin registries = registries_.pin();
    auto try_resolve = [&](const std::string& name) -> bool {
        if (registries.contains(models::RegistryDocument::Builtin, name)) {
            registry_name = name;
            is_user_lookup = false;
            return true;
        }
        if (auto canon = parse_canonical_id(name)) {
            if (canon->source == ModelSource::Builtin &&
                registries.contains(models::RegistryDocument::Builtin, canon->bare_name)) {
                registry_name = canon->bare_name;
                is_user_lookup = false;
                return true;
            }
            if (canon->source == ModelSource::Registered &&
                registries.contains(models::RegistryDocument::User, canon->bare_name)) {
                registry_name = canon->bare_name;
                is_user_lookup = true;
                return true;
            }
        }
        return false;
    };

    bool resolved = try_resolve(model_name);
    if (!resolved) {
        std::string canonical_name = resolve_model_name(model_name);
        if (canonical_name != model_name) {
            resolved = try_resolve(canonical_name);
        }
    }

    const json* model_json =
        registries.row(is_user_lookup ? models::RegistryDocument::User
                                      : models::RegistryDocument::Builtin,
                       registry_name);

    if (!model_json) {
        throw std::runtime_error("Model not found in registry: " + model_name);
    }

    info.model_name = is_user_lookup
        ? canonical_id(ModelSource::Registered, registry_name)
        : registry_name;
    models::parse_checkpoints(info, *model_json);
    models::parse_components(info, *model_json);
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", false);
    models::parse_model_source_fields(info, *model_json);
    info.system_prompt = JsonUtils::get_or_default<std::string>(*model_json, "system_prompt", "");

    // Parse labels array
    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            if (label.is_string()) {
                info.labels.push_back(label.get<std::string>());
            }
        }
    }
    // This path reads the registry json directly rather than the cache the
    // illegal entries were skipped from, so it refuses them again in its own
    // "no such model" terms.
    std::string illegal =
        lemon::backends::illegal_deployment_labels(info.labels, info.recipe);
    if (!illegal.empty()) {
        throw std::runtime_error(describe_illegal_labels(info.model_name, illegal));
    }
    lemon::backends::ensure_deployment_label(info.labels, info.recipe);

    // Parse size
    if (model_json->contains("size")) {
        if ((*model_json)["size"].is_number()) {
            info.size = (*model_json)["size"].get<double>();
        }
    }

    models::parse_extras(info, *model_json);

    return info;
}

std::string ModelManager::get_model_filter_reason(const std::string& model_name) {
    // Ensure cache is built (this populates the filter reasons)
    build_cache();

    auto snapshot = std::atomic_load(&store_snapshot_);
    std::string canonical_name = snapshot->canonical_for(model_name);
    if (canonical_name.empty()) {
        canonical_name = model_name;
    }

    // Model wasn't filtered out (either it's available or doesn't exist)
    return snapshot->rejection_reason(canonical_name);
}

std::set<std::string> ModelManager::recipes_with_all_models_filtered() {
    build_cache();
    auto snapshot = std::atomic_load(&store_snapshot_);
    const std::vector<std::string> hidden = snapshot->empty_recipes();
    return std::set<std::string>(hidden.begin(), hidden.end());
}

// Must be called with models_cache_mutex_ held.
//
// Populates two maps that drive the friendly-name / canonical-ID system:
//
//   public_model_aliases_ - input alias → cache key (canonical name in cache):
//     - <bare> → cache key of the precedence-winner for that bare name
//     - builtin.<X> → bare cache key X (built-ins are keyed bare in the cache)
//     - ModelInfo::input_aliases entries → cache key, without changing API output
//     - user.<X>, extra.<X> resolve directly via cache lookup fallback in the
//       callers, so no identity entries are required here
//
//   canonical_public_names_ - cache key → wire-format ID emitted by the API:
//     - winner cache keys → bare name, EXCEPT a non-built-in collection winner
//       (user.* / extra.* collection.omni / collection.router) → its canonical-
//       prefixed ID, so the listed ID matches the one used for registration,
//       fetch, and chat (the bare name still resolves via public_model_aliases_)
//     - shadowed cache keys → canonical-prefixed ID (user.X / extra.X / builtin.X)
//
// Precedence: Registered > Imported > Builtin. The algorithm itself is
// compute_model_alias_maps(), shared with build_cache()'s pre-filter
// resolver snapshot (#2748); this just runs it against models_cache_ and
// commits the result to member state.
void ModelManager::rebuild_public_model_aliases_locked() {
    ModelAliasMaps computed = compute_model_alias_maps(models_cache_);
    public_model_aliases_ = std::move(computed.public_model_aliases);
    canonical_public_names_ = std::move(computed.canonical_public_names);
}

} // namespace lemon
