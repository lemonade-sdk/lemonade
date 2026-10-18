#include <lemon/models/store.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/canonical_id.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/catalog_set.h>
#include <lemon/models/composition.h>
#include <lemon/models/entry_identity.h>
#include <lemon/models/model_info.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/aixlog.hpp>

#include <atomic>

namespace lemon {
namespace models {

ModelStore::ModelStore() : catalogs_(std::make_unique<CatalogSet>()) {
    // Readers must always have a snapshot; an invalid one tells them to build.
    auto empty = std::make_shared<StoreSnapshot>();
    std::atomic_store(&snapshot_, std::const_pointer_cast<const StoreSnapshot>(empty));
}

ModelStore::~ModelStore() = default;

std::shared_ptr<const StoreSnapshot> ModelStore::read() const {
    return std::atomic_load(&snapshot_);
}

std::vector<CatalogDescriptor> ModelStore::descriptors() const {
    return catalogs_->descriptors();
}

std::uint64_t ModelStore::generation() const { return read()->generation; }

bool ModelStore::cache_valid_locked() const { return cache_valid_; }

ModelStore::Write::Write(ModelStore& store) : store_(store), lock_(store.writer_mutex_) {
    // The write works on a copy of the set readers hold and the publish moves that copy
    // into the new snapshot: a published set is never mutated in place, and no second
    // copy of it lives on between writes.
    store_.draft_ = store.read()->models;
}

std::map<std::string, ModelInfo>& ModelStore::Write::draft() { return store_.draft_; }

bool ModelStore::Write::cache_valid() const { return store_.cache_valid_locked(); }

void ModelStore::Write::invalidate_cache() { store_.cache_valid_ = false; }

ModelStore::Write::~Write() {
    // Deriving the tables can fail; readers keep the previous snapshot rather than
    // unwinding an exception out of a destructor.
    try {
        // A patch says nothing about hardware fit, so the table already published
        // is the one that carries forward.
        store_.publish_locked(std::move(store_.draft_), store_.read()->reasons);
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelStore") << "Could not publish model store: " << e.what() << std::endl;
    }
}

ModelStore::Build::Build(ModelStore& store) : store_(store) {
    // Probed before taking the builder: publishing on a read that changes nothing would
    // leave the generation useless as a change signal.
    {
        std::lock_guard<std::mutex> probe(store.writer_mutex_);
        if (store.cache_valid_locked()) {
            up_to_date_ = true;
            return;
        }
        based_on_ = store.generation_;
    }

    // Composing reads catalogs, disk and backends without touching the published set, so
    // it takes a builder lock rather than the writer lock: holding the writer across it
    // made every writer wait for work it guards nothing of.
    builder_ = std::unique_lock<std::mutex>(store.builder_mutex_);
    std::lock_guard<std::mutex> probe(store.writer_mutex_);
    if (store.cache_valid_locked()) {
        up_to_date_ = true;  // Another rebuild published while this one waited.
        return;
    }
    based_on_ = store.generation_;
}

ModelStore::Build::~Build() = default;

bool ModelStore::Build::commit(std::map<std::string, ModelInfo>&& composed,
                               std::shared_ptr<const FilterReasons> reasons,
                               std::vector<std::shared_ptr<ModelCatalog>> catalogs) {
    bool landed = false;
    {
        std::lock_guard<std::mutex> writer(store_.writer_mutex_);
        if (store_.generation_ == based_on_) {
            // One swap, so a reader never sees a catalog set half-rebuilt.
            store_.catalogs_->replace_with(std::move(catalogs));
            store_.cache_valid_ = true;
            store_.publish_locked(std::move(composed), std::move(reasons));
            landed = true;
        }
    }
    // The compose is finished either way, so the next one may start. Holding the builder
    // past this point would make a store that just published refuse to be rebuilt.
    if (builder_.owns_lock()) {
        builder_.unlock();
    }
    if (!landed) {
        // A writer published while this composed, so the candidate predates that change
        // and cannot simply land on top of it. Yield: the write stays published, the
        // store stays invalid, and the next read asks again -- which reads the registries
        // and disk the writer wrote.
        LOG(INFO, "ModelStore")
            << "Model set changed while composing a new one; the next read will rebuild"
            << std::endl;
    }
    return landed;
}

void ModelStore::publish_locked(std::map<std::string, ModelInfo> models,
                                std::shared_ptr<const FilterReasons> reasons) {
    auto next = std::make_shared<StoreSnapshot>();
    next->models = std::move(models);
    // Publishing derives the alias tables and the helper projection from the set
    // published beside them, so a name cannot outlive the model it points at.
    ModelAliasMaps aliases = compute_model_alias_maps(next->models);
    next->public_model_aliases = std::move(aliases.public_model_aliases);
    next->canonical_public_names = std::move(aliases.canonical_public_names);
    next->reasons = std::move(reasons);
    next->catalogs = catalogs_->descriptors();
    next->helper_models = collect_helper_models(next->models);
    next->valid = cache_valid_;
    next->generation = ++generation_;
    std::atomic_store(&snapshot_, std::const_pointer_cast<const StoreSnapshot>(next));
}


// A compose that a concurrent write predated yields rather than clobbering it, and
// the caller asked about state the writer already persisted. Composing again answers
// that; returning here would hand back a set that predates the write.
void ModelStore::ensure_current(const std::function<CompositionResult()>& compose) {
    constexpr int kComposeAttempts = 4;
    for (int attempt = 0; attempt < kComposeAttempts; ++attempt) {
        ModelStore::Build build(*this);
        if (build.up_to_date()) {
            return;
        }

        LOG(INFO, "ModelStore") << "Building models cache..." << std::endl;

        CompositionResult composed = compose();

        if (build.commit(std::move(composed.models), std::move(composed.reasons),
                         std::move(composed.catalogs))) {
            LOG(INFO, "ModelStore") << "Cache built: " << read()->models.size()
                                    << " total, " << composed.downloaded << " downloaded"
                                    << std::endl;
            return;
        }
    }

    LOG(WARNING, "ModelStore")
        << "Model set kept changing mid-compose; the store stays invalid for the next read"
        << std::endl;
}

void ModelStore::patch_options(std::map<std::string, ModelInfo>& draft, const ModelInfo& info) {
    auto it = draft.find(info.model_name);
    if (it != draft.end()) {
        it->second.recipe_options = info.recipe_options;
    } else {
        LOG(WARNING, "ModelStore") << "'" << info.model_name << "' not found in cache" << std::endl;
    }
}


void ModelStore::patch_removed(const std::string& model_name) {
    ModelStore::Write store_write(*this);

    if (!store_write.cache_valid()) {
        return;
    }

    auto it = store_write.draft().find(model_name);
    if (it != store_write.draft().end()) {
        // User models and local uploads should be removed entirely from cache
        // (they're not in server_models.json, so keeping them makes no sense)
        bool is_user_model = is_user_model_name(model_name);
        if (is_user_model || it->second.source == "local_upload") {
            store_write.draft().erase(model_name);
            LOG(INFO, "ModelStore") << "Removed '" << model_name << "' from cache" << std::endl;
        } else {
            it->second.downloaded = false;
            it->second.update_available = false;
            LOG(INFO, "ModelStore") << "Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}


void ModelStore::patch_download(const ModelArtifactStore& artifacts,
                              const std::string& model_name, bool downloaded) {
    // Everything here is filesystem work, so it runs against the published view on
    // a private copy and only the rows it changed are committed.
    auto view = this->read();
    if (!view || !view->valid) {
        return;  // Will rebuild on next access
    }
    const ModelInfo* found = view->find(model_name);
    if (!found) {
        LOG(WARNING, "ModelStore") << "'" << model_name << "' not found in cache" << std::endl;
        return;
    }

    ModelInfo patched = *found;
    patched.downloaded = downloaded;
    if (downloaded) {
        // What just arrived cannot also be behind.
        patched.update_available = false;
        artifacts.resolve_all(patched);
    } else {
        patched.max_context_window = 0;
    }
    const bool backend_lists_models =
        downloaded && backends::ops_for(patched.recipe)->invalidates_cache_after_download();
    if (!backend_lists_models) {
        populate_model_metadata(patched, artifacts);
        ModelArtifactStore::refresh_on_disk_size(patched);
    }

    // Collections carrying this model follow it without a full rebuild.
    const auto dependents =
        collection_states_after_component(view->models, model_name, downloaded);
    const std::string resolved = patched.resolved_path();

    ModelStore::Write store_write(*this);
    if (!store_write.cache_valid()) {
        return;  // Something else rebuilt while this ran, and it read the disk again
    }
    if (auto it = store_write.draft().find(model_name); it != store_write.draft().end()) {
        // Options are patched into the live row without invalidating, so a save that
        // landed while this read the disk keeps its answer.
        patched.recipe_options = it->second.recipe_options;
        it->second = std::move(patched);
    }
    for (const auto& [name, state] : dependents) {
        auto it = store_write.draft().find(name);
        if (it == store_write.draft().end() || it->second.downloaded == state) continue;
        it->second.downloaded = state;
        LOG(INFO, "ModelStore") << "Collection '" << name << "' downloaded=" << state
                                 << " (dependent on " << model_name << ")" << std::endl;
    }
    if (backend_lists_models) {
        store_write.invalidate_cache();
        LOG(INFO, "ModelStore") << "Invalidated model cache after download for '" << model_name
                                 << "' (backend rebuilds its model list)" << std::endl;
        return;
    }
    LOG(INFO, "ModelStore") << "Updated '" << model_name << "' downloaded=" << downloaded
                             << ", resolved_path=" << resolved << std::endl;
}


}  // namespace models
}  // namespace lemon
