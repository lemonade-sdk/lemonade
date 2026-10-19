#pragma once

#include <lemon/models/catalog_descriptor.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;

namespace models {

class ModelArtifactStore;
struct CompositionResult;
struct FilterReasons;
struct StoreSnapshot;
class CatalogSet;
class ModelCatalog;

// The published model set, and the only ways to read it or change it.
//
// A read takes one shared_ptr and never waits: publishing swaps a new snapshot in rather
// than editing what readers hold. A change either patches -- one writer at a time, on the
// draft the last publish left -- or composes a whole set, which is slow enough (catalogs,
// disk, backends) that it runs outside the writer and commits only if nothing was written
// in the meantime.
class ModelStore {
public:
    ModelStore();
    ~ModelStore();

    // Never null. `valid` false means the set is empty and the caller should build.
    std::shared_ptr<const StoreSnapshot> read() const;

    // What the last build composed, in precedence order.
    std::vector<CatalogDescriptor> descriptors() const;

    // Rises on every publish, so a slow read can tell whether what it started from is
    // still current.
    std::uint64_t generation() const;

    // Marks the published set stale: the next read composes a new one. Nothing is copied
    // and nothing is published here, so invalidating twice and rebuilding once bumps the
    // generation once -- from the rebuild that changed the set, which is what a
    // generation is for.
    void invalidate();

    // A patch: one writer at a time, published when the scope ends. The draft starts as
    // the published set, so a change edits rows and leaves the rest alone.
    class Write {
      public:
        explicit Write(ModelStore& store);
        Write(const Write&) = delete;
        Write& operator=(const Write&) = delete;
        ~Write();

        std::map<std::string, ModelInfo>& draft();

        // False when there is nothing published to patch, which the next read builds.
        bool cache_valid() const;
        // For a change nothing here can patch: the next read composes a set afresh.
        void invalidate_cache();

      private:
        ModelStore& store_;
        std::unique_lock<std::mutex> lock_;
    };

    // A compose: probes on construction, and holds the builder while a candidate is
    // being put together. Committing installs the set with the catalogs that produced it;
    // it refuses when a write landed after the probe, because the candidate predates that
    // change and cannot simply land on top of it.
    class Build {
      public:
        explicit Build(ModelStore& store);
        Build(const Build&) = delete;
        Build& operator=(const Build&) = delete;
        ~Build();

        // True when the published set already answers: compose nothing.
        bool up_to_date() const { return up_to_date_; }

        bool commit(std::map<std::string, ModelInfo>&& composed,
                    std::shared_ptr<const FilterReasons> reasons,
                    std::vector<std::shared_ptr<ModelCatalog>> catalogs);

      private:
        ModelStore& store_;
        std::uint64_t based_on_ = 0;
        bool up_to_date_ = false;
        std::unique_lock<std::mutex> builder_;
    };

    // The compose loop, where "build the set if it is stale" lives. A compose that a
    // concurrent write predated yields and tries again rather than clobbering it.
    void ensure_current(const std::function<CompositionResult()>& compose);

    // Patches: bringing the published set in line with one thing that changed. Each
    // reads disk against the published view and takes the writer only for the rows.

    // A model's bytes appeared or went away; collections routing through it follow.
    void patch_download(const ModelArtifactStore& artifacts, const std::string& model_name,
                        bool downloaded);

    // Gone rather than misplaced: a registration leaves the set, a built-in stays.
    void patch_removed(const std::string& model_name);

    // Onto the caller's write scope, so a saved file and its row are never apart.
    static void patch_options(std::map<std::string, ModelInfo>& draft, const ModelInfo& info);

  private:
    friend class ModelStoreTest;

    // Derives the tables beside the set. The source is emptied: a published set is
    // never mutated in place afterwards.
    void publish_locked(std::map<std::string, ModelInfo> models,
                        std::shared_ptr<const FilterReasons> reasons);
    bool cache_valid_locked() const;

    mutable std::mutex writer_mutex_;
    std::mutex builder_mutex_;
    std::map<std::string, ModelInfo> draft_;
    std::shared_ptr<const StoreSnapshot> snapshot_;
    std::unique_ptr<CatalogSet> catalogs_;
    std::uint64_t generation_ = 0;
    bool cache_valid_ = false;
};

}  // namespace models
}  // namespace lemon
