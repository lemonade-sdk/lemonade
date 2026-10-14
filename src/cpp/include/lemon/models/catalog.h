#pragma once

#include <lemon/model_manager.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// One catalog's contribution. `id` is stable, used for provenance.
struct CatalogEntry {
    std::string catalog_id;
    ModelInfo info;
};

// Collects entries from several catalogs; precedence lives here, not per source.
class CatalogAccumulator {
  public:
    enum class Conflict { KeepExisting, Overwrite };
    enum class Outcome { Added, KeptExisting };

    CatalogAccumulator() = default;

    // For a source that claimed its names before enumeration began, such as the
    // JSON registries.
    explicit CatalogAccumulator(std::map<std::string, ModelInfo> already_claimed)
        : models_(std::move(already_claimed)) {}

    // The key is the entry's model name, which every source already sets.
    Outcome add(CatalogEntry entry, Conflict conflict = Conflict::KeepExisting);

    const std::map<std::string, ModelInfo>& models() const { return models_; }

    std::map<std::string, ModelInfo> take() { return std::move(models_); }

    bool contains(const std::string& id) const { return models_.count(id) != 0; }

    size_t size() const { return models_.size(); }

  private:
    std::map<std::string, ModelInfo> models_;
};

// A source of model entries; enumeration is the only way one contributes.
class ModelCatalog {
  public:
    virtual ~ModelCatalog() = default;

    virtual const std::string& id() const = 0;

    // True when this catalog's failures should stop the build rather than
    // contribute nothing.
    virtual bool required() const { return false; }

    virtual void enumerate(CatalogAccumulator& accumulator) const = 0;
};

// Wraps a callable, so a catalog over a backend or provider needs no include of
// that layer. The callable sees what earlier catalogs contributed.
class EnumeratedCatalog : public ModelCatalog {
  public:
    using Discover = std::function<std::vector<ModelInfo>(const CatalogAccumulator&)>;
    using OnConflict = std::function<void(const std::string& model_name)>;

    // `on_conflict` reports an entry lost to one already claimed; whether to say
    // anything is the catalog's policy.
    EnumeratedCatalog(std::string id, Discover discover, OnConflict on_conflict = nullptr,
                      bool required = false)
        : id_(std::move(id)),
          discover_(std::move(discover)),
          on_conflict_(std::move(on_conflict)),
          required_(required) {}

    const std::string& id() const override { return id_; }
    bool required() const override { return required_; }

    // A source that cannot answer contributes nothing, so an unconfigured catalog
    // cannot hide configured models. A required catalog stops the build.
    void enumerate(CatalogAccumulator& accumulator) const override;

  private:
    std::string id_;
    Discover discover_;
    OnConflict on_conflict_;
    bool required_;
};

}  // namespace models
}  // namespace lemon
