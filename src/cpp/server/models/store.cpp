#include <lemon/models/store.h>

#include <lemon/models/model_info.h>
#include <lemon/models/catalog_set.h>
#include <lemon/models/entry_identity.h>
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

}  // namespace models
}  // namespace lemon
