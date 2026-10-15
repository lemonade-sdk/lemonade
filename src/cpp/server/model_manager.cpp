#include <lemon/model_manager.h>
#include <lemon/runtime_config.h>
#include <lemon/hf_variants.h>
#include <lemon/model_registry.h>
#include <lemon/models/artifact_resolver.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/catalog.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/definition_labels.h>
#include <lemon/models/ingest.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/models/scan_catalog.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/models/routing_parse_options.h>
#include <lemon/models/model_index.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/model_name_utils.h>
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

// Built-ins are keyed bare in models_cache_; user.* and extra.* keys already
// include their canonical prefix. This helper returns the canonical ID for any
// cache key, which is the form used by recipe_options.json on disk.
static std::string cache_key_to_canonical_id(const std::string& cache_key) {
    if (parse_canonical_id(cache_key)) {
        return cache_key;
    }
    return canonical_id(ModelSource::Builtin, cache_key);
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

static bool is_extra_model_name(const std::string& model_name) {
    return model_name.rfind(models::extra_model_prefix, 0) == 0;
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
    return ingest_->download_source_for(info);
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

models::ModelIngest::Deps ModelManager::make_ingest_deps() {
    models::ModelIngest::Deps deps;
    deps.artifacts = artifacts_.get();
    deps.default_registry_source = [this] {
        if (!default_model_source_provider_) return std::optional<RegistryId>{};
        return std::make_optional(parse_remote_registry_source(default_model_source_provider_()));
    };
    deps.model_exists = [this](const std::string& name) { return model_exists(name); };
    deps.model_exists_unfiltered =
        [this](const std::string& name) { return model_exists_unfiltered(name); };
    deps.model_info = [this](const std::string& name) { return get_model_info(name); };
    deps.filter_reason = [this](const std::string& name) { return get_model_filter_reason(name); };
    deps.resolve_name = [this](const std::string& name) { return resolve_model_name(name); };
    deps.is_downloaded = [this](const std::string& name) { return is_model_downloaded(name); };
    deps.resolved_paths = [this](const std::string& name) {
        StoreWrite store_write(*this);
        std::map<std::string, std::filesystem::path> paths;
        const auto model = models_cache_.find(name);
        if (model != models_cache_.end()) {
            for (const auto& [type, path] : model->second.resolved_paths) {
                paths[type] = path_from_utf8(path).lexically_normal();
            }
        }
        return paths;
    };
    deps.register_model = [this](const std::string& name, const json& model_data) {
        register_user_model(name, model_data);
    };
    deps.unregister_model = [this](const std::string& name) { unregister_user_model(name); };
    deps.patch_downloaded = [this](const std::string& name, bool downloaded) {
        update_model_in_cache(name, downloaded);
    };
    deps.invalidate = [this] { invalidate_models_cache(); };
    deps.persist_size = [this](const std::string& canonical_model_name) {
        StoreWrite store_write(*this);
        const auto model = models_cache_.find(canonical_model_name);
        if (model == models_cache_.end()) {
            return;
        }
        const bool wrote = registries_.update_user([&](json& user_models) {
            auto entry = user_models.find(strip_user_model_prefix(canonical_model_name));
            if (entry == user_models.end()) {
                return false;
            }
            (*entry)["size"] = model->second.size;
            return true;
        });
        if (wrote) {
            cache_valid_ = false;
        }
    };
    deps.pin = [this] { return registries_.pin(); };
    deps.save_options = [this](const ModelInfo& info) { save_model_options(info); };
    deps.model_updated = [this](const std::string& canonical_model_name) {
        change_notifier_.model_updated(canonical_model_name);
    };
    return deps;
}

ModelManager::ModelManager(const std::string& extra_models_dir)
    : extra_models_dir_(extra_models_dir),
      alias_manager_(utils::get_cache_dir()),
      registries_(load_server_models(), get_user_models_file()),
      recipe_options_(get_recipe_options_file()),
      artifacts_(std::make_unique<models::ModelArtifactStore>(models::ModelArtifactStore::Hooks{
          [this] { return get_hf_cache_dir(); },
          [this](const ModelInfo& info, const std::string& type) { return source_for(info, type); },
          [this] { return extra_models_dir_; },
      })),
      ingest_(std::make_unique<models::ModelIngest>(make_ingest_deps())),
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

void ModelManager::download_from_huggingface_engine(
        const ModelInfo& info, DownloadProgressCallback progress_callback) {
    ingest_->fetch_registry_artifacts(info, progress_callback);
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
        ingest_->populate_from_cached_manifest(info);
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

    // A scanning or listing source keeps what an earlier source already claimed.
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
        octx.fetch = ingest_.get();
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
    status_ctx.fetch = ingest_.get();
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
        octx.fetch = ingest_.get();
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

    // A registry-backed collection persists only its repo pointer, so a refreshed
    // manifest cannot be shadowed by a stale local list.
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
    ingest_->fetch_model(info, do_not_upgrade, progress_callback);
}

void ModelManager::register_model(const std::string& model_name,
                                 const json& model_data,
                                 bool allow_missing_checkpoint,
                                 bool replace_existing) {
    ingest_->register_model(model_name, model_data, allow_missing_checkpoint,
                            replace_existing);
}

void ModelManager::download_model(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback) {
    if (download_model_override_) {
        download_model_override_(model_name, model_data, do_not_upgrade, progress_callback);
        return;
    }
    ingest_->pull(model_name, model_data, do_not_upgrade, progress_callback);
}

std::optional<models::ResolvedModel> ModelManager::resolve(const std::string& requested) {
    build_cache();
    auto snapshot = std::atomic_load(&store_snapshot_);

    std::string name = utils::normalize_model_name(requested);
    constexpr const char kLatest[] = ":latest";
    if (name.size() > strlen(kLatest) &&
        name.compare(name.size() - strlen(kLatest), strlen(kLatest), kLatest) == 0) {
        name.erase(name.size() - strlen(kLatest));
    }

    // A snapshot name wins over an alias, so an alias can never hide a model.
    const auto from_snapshot = [&snapshot](const std::string& candidate) {
        const std::string canonical = snapshot->canonical_for(candidate);
        const ModelInfo* found = snapshot->find(canonical.empty() ? candidate : canonical);
        if (found == nullptr) return std::pair<std::string, const ModelInfo*>{};
        return std::pair<std::string, const ModelInfo*>{canonical.empty() ? candidate : canonical,
                                                        found};
    };

    auto hit = from_snapshot(name);
    bool via_alias = false;
    if (hit.second == nullptr) {
        const std::string target = alias_manager_.resolve_alias(name).value_or("");
        if (target.empty()) return std::nullopt;
        hit = from_snapshot(target);
        if (hit.second == nullptr) return std::nullopt;
        via_alias = true;
    }

    models::ResolvedModel resolved;
    resolved.canonical_id = hit.first;
    resolved.info = std::shared_ptr<const ModelInfo>(snapshot, hit.second);
    resolved.via_alias = via_alias;
    resolved.wire_id = via_alias
                           ? name
                           : get_public_model_name(hit.first);
    return resolved;
}

std::map<std::string, std::string> ModelManager::aliases() const {
    return alias_manager_.get_all_aliases();
}

bool ModelManager::set_alias(const std::string& alias, const std::string& target,
                             std::string& err_msg) {
    return alias_manager_.set_alias(alias, target, err_msg);
}

bool ModelManager::remove_alias(const std::string& alias) { return alias_manager_.remove_alias(alias); }

std::optional<std::string> ModelManager::resolve_alias(const std::string& alias_or_name) const {
    return alias_manager_.resolve_alias(alias_or_name);
}

bool ModelManager::has_alias(const std::string& alias) const { return alias_manager_.has_alias(alias); }

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
    return ingest_->validate_collection(model_name, model_data);
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
