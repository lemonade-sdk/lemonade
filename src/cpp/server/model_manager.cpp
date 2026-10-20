#include <lemon/model_manager.h>
#include <lemon/runtime_config.h>
#include <lemon/hf_variants.h>
#include <lemon/models/registry_types.h>
#include <lemon/canonical_id.h>
#include <lemon/directory_watcher.h>
#include <lemon/models/artifact_resolver.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/catalog.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/definition_labels.h>
#include <lemon/models/entry_identity.h>
#include <lemon/models/host_availability.h>
#include <lemon/models/ingest.h>
#include <lemon/models/composition.h>
#include <lemon/models/catalog_set.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/models/model_sources.h>
#include <lemon/models/recipe_option_layers.h>
#include <lemon/models/scan_catalog.h>
#include <lemon/models/store.h>
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
#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/backends/backend_utils.h>
#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/backends/hf_cache_util.h>
#include <lemon/cloud_provider_registry.h>
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

// Candidate roots that FLM may use to store models. FLM resolves its model
// directory from the FLM_MODEL_PATH env var (set by the installer) and falls
// back to a built-in default that has changed across releases. lemond is often
// launched from a parent process that predates the FLM install and therefore
// doesn't see FLM_MODEL_PATH, so we also probe every documented default.
// Order is most-specific to most-historical.

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

std::unique_ptr<models::ModelComposition> ModelManager::make_composition() {
    models::ModelComposition::Deps deps;
    deps.load_catalogs = [this](std::map<std::string, json>& authored, models::CatalogSet& built) {
        built.replace_with(models::product_catalogs(make_catalog_deps(authored)));
        for (const auto& catalog : config_.extra_catalogs) {
            built.add(catalog);
        }
    };
    deps.saved_options = [this] { return recipe_options_.all(); };
    deps.availability = config_.availability;
    deps.fetch = ingest_.get();
    deps.artifacts = artifacts_.get();
    return std::make_unique<models::ModelComposition>(std::move(deps));
}

std::vector<ModelFileInfo> ModelManager::list_model_files(const std::string& model_name) {
    return artifacts_->list_files(get_model_info(model_name));
}

json ModelManager::model_sources() {
    build_cache();
    return models::model_sources_json(*store_.read());
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
    deps.store = &store_;
    return deps;
}

models::ModelIngest::Deps ModelManager::make_ingest_deps() {
    models::ModelIngest::Deps deps;
    deps.repository_for = config_.repository_for;
    deps.artifacts = artifacts_.get();
    deps.default_registry_source = [this] {
        if (!default_model_source_provider_) return std::optional<RegistryId>{};
        return std::make_optional(parse_remote_registry_source(default_model_source_provider_()));
    };
    deps.registries = &registries_;
    deps.store = &store_;
    deps.notifier = &change_notifier_;
    deps.save_options = [this](const ModelInfo& info) { save_model_options(info); };
    return deps;
}

ModelManager::ModelManager(const std::string& extra_models_dir)
    : ModelManager(ModelManagerConfig{{}, extra_models_dir, nullptr, {}, {}}) {}

ModelManager::ModelManager(ModelManagerConfig config)
    : config_(std::move(config)),
      extra_models_dir_(config_.extra_models_dir),
      alias_manager_(config_.cache_dir.empty() ? utils::get_cache_dir() : config_.cache_dir),
      registries_(load_server_models(), get_user_models_file()),
      recipe_options_(get_recipe_options_file()),
      store_(alias_manager_, [this] { return composition_->run(); }),
      artifacts_(std::make_unique<models::ModelArtifactStore>(models::ModelArtifactStore::Hooks{
          [this] { return get_hf_cache_dir(); },
          [this](const ModelInfo& info, const std::string& type) { return source_for(info, type); },
          [this] { return extra_models_dir_; },
      })),
      ingest_(std::make_unique<models::ModelIngest>(make_ingest_deps())),
      composition_(make_composition()),
      sync_(make_sync_deps()),
      cloud_rows_(store_, [this](const std::string& provider) {
          return backends::CloudServer::discover_provider_models(*cloud_registry_, provider);
      }) {
    cloud_registry_ = config_.cloud_providers;
    default_model_source_provider_ = std::move(config_.default_model_source);

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

void ModelManager::invalidate_models_cache() { store_.invalidate(); }

void ModelManager::on_models_changed(std::function<void(std::uint64_t)> subscriber) {
    change_notifier_.on_models_changed(std::move(subscriber));
}

void ModelManager::on_model_updated(std::function<void(const std::string&)> subscriber) {
    change_notifier_.on_model_updated(std::move(subscriber));
}

std::set<std::string> ModelManager::active_helper_models() {
    build_cache();
    return store_.read()->helper_models;
}

void ModelManager::notify_models_changed() {
    // The snapshot a subscriber must read has to exist first. See
    // models/change_notifier.h for what the generation means to it.
    build_cache();
    change_notifier_.models_changed(store_.read()->generation);
}

void ModelManager::set_extra_models_dir(const std::string& dir) {
    extra_models_dir_ = dir;

    directory_watcher_.reset();

    if (!extra_models_dir_.empty()) {
        LOG(INFO, "ModelManager") << "Extra models directory set to: " << extra_models_dir_ << std::endl;
        start_directory_watcher();
    }

    catalog_set_changed();
}

void ModelManager::catalog_set_changed() {
    // The set itself moved, so every origin is asked again.
    catalog_fetch_clock_->forget_all();
    store_.invalidate();
    // The one generation bump this path makes is the rebuild below, which is the moment
    // the published set actually changed.
    notify_models_changed();
}

ModelManager::~ModelManager() {
    directory_watcher_.reset();
}

void ModelManager::start_directory_watcher() {
    directory_watcher_ = std::make_unique<DirectoryWatcher>(extra_models_dir_);
    directory_watcher_->set_callback([this]() {
        LOG(DEBUG, "ModelManager") << "Extra models directory changed, invalidating cache" << std::endl;
        store_.invalidate();
        notify_models_changed();
    });
    directory_watcher_->start();
}

// Apply the default only after discovery has checked for an explicit directory mode.

std::map<std::string, ModelInfo> ModelManager::discover_extra_models(
    const std::set<fs::path>& registered_paths) const {
    return models::ScanCatalog::for_dir(extra_models_dir_).enumerate(registered_paths);
}

std::vector<models::CheckpointStatus> ModelManager::checkpoint_status(const ModelInfo& info) const {
    std::vector<models::CheckpointStatus> statuses;
    statuses.reserve(info.checkpoints.size());
    for (const auto& [type, checkpoint] : info.checkpoints) {
        models::CheckpointStatus status;
        status.type = type;
        try {
            const std::unique_ptr<models::ArtifactSource> source = source_for(info, type);
            status.required = source->ref().required;
            status.registry = source->ref().registry.value_or(RegistryId{});
            const models::Presence presence = source->present();
            status.present = presence.complete;
            status.path = presence.path;
        } catch (const std::exception&) {
            // A source that cannot even be built holds nothing; say present=false
            // rather than failing the listing.
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

std::unique_ptr<models::ArtifactSource> ModelManager::source_for(
    const ModelInfo& info, const std::string& checkpoint_type) const {
    models::ArtifactSourceResolver::Hooks hooks;
    hooks.download_policy = [this](const ModelInfo& info) { return download_policy(info); };
    hooks.repository_for = [this](const RegistryId& host, const models::ArtifactRef& ref,
                                   const models::SourcePlacement& placement) {
        return config_.repository_for ? config_.repository_for(ref)
                                      : models::create_repository(host, ref, placement);
    };

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
    models::ModelStore::patch_options(store_write.draft(), info);
}

json ModelManager::options(const std::string& name, const json& changes) {
    const std::string cache_key = resolve_model_name(name);
    const std::string id = cache_key_to_canonical_id(cache_key);
    const json saved = recipe_options_.entry(id);

    json applied = saved;
    if (changes.is_object()) {
        for (const auto& [key, value] : changes.items()) {
            if (value.is_null()) {
                applied.erase(key);
            } else {
                applied[key] = value;
            }
        }
    }

    json defaults = nullptr;
    json resolved = nullptr;
    try {
        const ModelInfo info = get_model_info(cache_key);
        defaults = models::build_recipe_options(
                       info, registry_recipe_options(cache_key), "", json::object())
                       .to_json();
        // Same layering as write_saved_model_options, so a read of what changes
        // would produce cannot differ from the state the write lands.
        resolved = models::build_recipe_options(
                       info, registry_recipe_options(cache_key), id, json{{id, applied}})
                       .to_json();
    } catch (const std::exception&) {
    }
    return {{"saved", saved}, {"defaults", defaults}, {"resolved_with", resolved}};
}

json ModelManager::registry_recipe_options(const std::string& cache_key) {
    const models::RegistryDocuments::Pin rows = registries_.pin();
    const json* row = registry_row(rows, cache_key);
    return row ? models::row_recipe_options(*row) : json(nullptr);
}

json ModelManager::replace_options(const std::string& name, const json& saved) {
    return write_saved_model_options(name, saved, /*merge=*/false);
}

json ModelManager::set_options(const std::string& name, const json& changes) {
    return write_saved_model_options(name, changes, /*merge=*/true);
}

// Read-modify-write of one model's recipe_options.json entry.
//
// recipe_options_write_mutex_ orders the whole-file rewrites, so two writers
// can't land their snapshots out of order; the store's writer covers the merge
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
        info.recipe_options = models::build_recipe_options(
            info, registry_recipe_options(cache_key), id, json{{id, saved}});
        models::ModelStore::patch_options(store_write.draft(), info);
    } else {
        invalidate_models_cache();
    }

    return saved;
}

std::shared_ptr<const models::StoreSnapshot> ModelManager::snapshot() {
    build_cache();
    return store_.read();
}

std::map<std::string, ModelInfo> ModelManager::get_supported_models() {
    return store_.supported_models();
}

ModelManager::UpdateCheckResult ModelManager::check_updates(const std::vector<std::string>& targets) {
    return sync_.scan_for_updates(targets);
}

// The scan lives in models/update_scan.cpp. What stays here is what the scan may
// not reach: the store it reads, the provenance file behind its fast path, and
// the one store write that publishes the flags it settled.
ModelManager::UpdateCheckResult ModelManager::run_update_scan(const std::vector<std::string>& targets) {
    models::UpdateScanDeps deps;
    deps.artifacts = artifacts_.get();
    deps.ensure_store = [this] { build_cache(); };
    deps.snapshot = [this] { return store_.read(); };
    deps.repository_for = config_.repository_for;
    deps.cancel_requested = [this] { return sync_.cancel_requested(); };
    deps.publish = [this](const models::ScanDetermination& settled,
                          models::UpdateScanResult& result) {
        models::publish_scan_flags(store_, settled, result);
    };

    return models::run_update_scan(deps, targets);
}

bool ModelManager::should_auto_update(const ModelInfo& info) const {
    return sync_.should_auto_update(info);
}

json ModelManager::sync_models(const std::vector<std::string>& target_models, bool dry_run,
                               bool attach_if_running) {
    return sync_.sync(target_models, dry_run, attach_if_running);
}

json ModelManager::sync_status(uint64_t sync_id) const {
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

void ModelManager::download_from_huggingface_engine(
        const ModelInfo& info, DownloadProgressCallback progress_callback) {
    ingest_->fetch_registry_artifacts(info, progress_callback, {});
}

std::vector<ModelInfo> ModelManager::scan_extra_models(
    const models::CatalogAccumulator& claimed) {
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
}

models::CatalogSetDeps ModelManager::make_catalog_deps(
    std::map<std::string, json>& json_recipe_options) {
    models::CatalogSetDeps deps;

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
    deps.registry_rows = std::move(rows);
    deps.pin_registries = [this] { return registries_.pin(); };
    deps.scan_extra_models = [this](const models::CatalogAccumulator& claimed) {
        return scan_extra_models(claimed);
    };

    deps.catalog_cache_dir =
        config_.cache_dir.empty() ? utils::get_cache_dir() : config_.cache_dir;
    deps.catalog_fetch_clock = catalog_fetch_clock_;
    if (config_.model_catalogs) {
        models::ParsedCatalogs parsed;
        try {
            parsed = models::parse_configured_catalogs(config_.model_catalogs());
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager") << "'model_catalogs' is not usable: " << e.what() << std::endl;
        }
        for (const auto& refusal : parsed.refusals) {
            LOG(ERROR, "ModelManager") << "Ignoring a configured catalog: " << refusal << std::endl;
        }
        deps.configured_catalogs = std::move(parsed.catalogs);
    }

    std::set<std::string> configured_ids;
    for (const auto& catalog : deps.configured_catalogs) {
        configured_ids.insert(catalog.id);
    }
    models::prune_cached_bodies(deps.catalog_cache_dir, configured_ids);

    auto* config = RuntimeConfig::global();
    deps.offline = config != nullptr && config->offline();
    deps.dynamic_recipes = [] {
        std::vector<std::string> recipes;
        for (const auto* desc : backends::all_descriptors()) {
            if (desc->dynamic_models) {
                recipes.push_back(desc->recipe);
            }
        }
        return recipes;
    };
    deps.discover_backend = [this](const std::string& recipe) {
        backends::BackendOpsContext octx;
        octx.fetch = ingest_.get();
        octx.artifacts = artifacts_.get();
        octx.cloud_registry = cloud_registry_;
        return backends::ops_for(recipe)->discover_models(octx);
    };
    deps.cloud_providers = [this] {
        std::vector<std::string> providers;
        if (cloud_registry_ != nullptr) {
            for (const auto& rec : cloud_registry_->list_installed()) {
                providers.push_back(rec.name);
            }
        }
        return providers;
    };
    deps.list_cloud_models = [this](const std::string& provider) {
        return backends::CloudServer::discover_provider_models(*cloud_registry_, provider);
    };
    return deps;
}

void ModelManager::build_cache() {
    store_.ensure_current([this] { return composition_->run(); });
}



std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    return store_.downloaded_models();
}

bool ModelManager::is_model_downloaded(const std::string& model_name) {
    return store_.is_downloaded(*artifacts_, model_name);
}

backends::DownloadPolicy ModelManager::download_policy(const ModelInfo& info) const {
    return artifacts_->download_policy(info);
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
                                 DownloadProgressCallback progress_callback,
                                 const utils::CancelToken& cancel) {
    ingest_->pull(model_name, model_data, do_not_upgrade, progress_callback, cancel);
}

std::string ModelManager::canonical_for_request(const std::string& requested) {
    auto resolved = resolve(requested);
    return resolved && resolved->via_alias ? resolved->canonical_id : requested;
}

std::vector<std::pair<std::string, const ModelInfo*>> ModelManager::listing_rows(
    const std::map<std::string, ModelInfo>& listed) {
    std::vector<std::pair<std::string, const ModelInfo*>> rows;
    rows.reserve(listed.size());
    for (const auto& [id, info] : listed) {
        rows.emplace_back(id, &info);
    }
    for (const auto& [alias_id, target] : alias_manager_.get_all_aliases()) {
        if (listed.count(alias_id) > 0) continue;
        const std::string alias_target = alias_manager_.resolve_alias(alias_id).value_or(target);
        auto row = listed.find(resolve_model_name(alias_target));
        if (row == listed.end()) row = listed.find(alias_target);
        if (row != listed.end()) rows.emplace_back(alias_id, &row->second);
    }
    return rows;
}

std::optional<models::ResolvedModel> ModelManager::resolve(const std::string& requested) {
    return store_.resolve(requested);
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

RegistrySearchResponse ModelManager::search_registry(const std::string& source_name,
                                                             const std::string& query,
                                                             std::size_t limit,
                                                             bool gguf_only) const {
    const auto source = parse_remote_registry_source(source_name);
    models::ArtifactRef probe;
    probe.registry = source;
    const auto repository =
        models::create_repository(source, probe, models::SourcePlacement{});
    if (!repository->supports_search()) {
        throw std::runtime_error(remote_registry_display_name(source) +
                                 " does not support model search");
    }
    return repository->search(query, limit, gguf_only);
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
    // Who else claims these bytes is a question about the published set, and that
    // set stays valid for the whole disk walk because this holds it.
    const auto published = store_.read();
    const models::DeleteReport report = artifacts_->delete_artifacts(info, published->models);

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
    store_.patch_removed(canonical_model_name);
}

void ModelManager::register_user_model(const std::string& model_name,
                                       const json& model_data,
                                       const std::string& source) {
    ingest_->register_user_model(model_name, model_data, source);
}

void ModelManager::unregister_user_model(const std::string& model_name) {
    ingest_->unregister_user_model(model_name);
}

void ModelManager::forget_registered_model(const std::string& canonical_model_name) {
    ingest_->forget_registered_model(canonical_model_name);
}

std::size_t ModelManager::refresh_cloud_models(const std::string& provider) {
    if (cloud_registry_ == nullptr) {
        return 0;
    }
    return cloud_rows_.refresh(provider);
}

std::size_t ModelManager::evict_cloud_models(const std::string& provider) {
    return cloud_rows_.evict(provider);
}

std::size_t ModelManager::count_cloud_models(const std::string& provider) const {
    return cloud_rows_.count(provider);
}

json ModelManager::cleanup_orphaned_cache(bool dry_run) {
    build_cache();

    const auto published = store_.read();
    const models::CleanupReport report =
        artifacts_->cleanup_orphaned_cache(published->models, dry_run);

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
    return *store_.info(model_name);
}

std::string ModelManager::resolve_model_name(const std::string& model_name) {
    return store_.resolve_name(model_name);
}

std::string ModelManager::get_public_model_name(const std::string& model_name) {
    return store_.public_name_for(model_name);
}

bool ModelManager::model_exists(const std::string& model_name) {
    return store_.exists(model_name);
}

std::optional<std::string> ModelManager::validate_collection_request(
    const std::string& model_name, const json& model_data) {
    return ingest_->validate_collection(model_name, model_data);
}

bool ModelManager::model_exists_unfiltered(const std::string& model_name) {
    const models::RegistryDocuments::Pin registries = registries_.pin();
    if (models::find_registry_row(registries, model_name)) {
        return true;
    }
    return static_cast<bool>(
        models::find_registry_row(registries, resolve_model_name(model_name)));
}

std::string ModelManager::get_model_filter_reason(const std::string& model_name) {
    return store_.rejection_reason(model_name);
}

std::set<std::string> ModelManager::recipes_with_all_models_filtered() {
    const std::vector<std::string> hidden = store_.current()->empty_recipes();
    return std::set<std::string>(hidden.begin(), hidden.end());
}

} // namespace lemon
