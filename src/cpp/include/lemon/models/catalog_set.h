#pragma once

#include <lemon/models/catalog.h>
#include <lemon/models/configured_catalog.h>
#include <lemon/models/json_catalog.h>
#include <lemon/models/registry_documents.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// The catalogs a store builds from. A build enumerates them in declared rank
// order, so a catalog that joins later takes its precedence from its descriptor
// instead of from where its construction happened to land.
// How a host becomes catalogs. The set decides which catalogs exist and in what
// precedence; the caller decides only how its own documents, directories and
// installed backends are read.
struct CatalogSetDeps {
    // Row policy shared by every JSON registry document; each catalog applies
    // what distinguishes it.
    RegistryRowRules registry_rows;
    std::function<RegistryDocuments::Pin()> pin_registries;
    std::function<std::vector<ModelInfo>(const CatalogAccumulator&)> scan_extra_models;

    std::vector<ConfiguredCatalog> configured_catalogs;
    std::string catalog_cache_dir;

    bool offline = false;
    std::function<std::vector<std::string>()> dynamic_recipes;
    std::function<std::vector<ModelInfo>(const std::string& recipe)> discover_backend;
    std::function<std::vector<std::string>()> cloud_providers;
    std::function<std::vector<ModelInfo>(const std::string& provider)> list_cloud_models;
};

// The catalogs the shipped product builds from.
std::vector<std::shared_ptr<ModelCatalog>> product_catalogs(const CatalogSetDeps& deps);

class CatalogSet {
  public:
    CatalogSet() = default;

    // A catalog with the same id is replaced.
    void add(std::shared_ptr<ModelCatalog> catalog);
    bool remove(const std::string& id);

    std::shared_ptr<ModelCatalog> find(const std::string& id) const;
    size_t size() const;
    bool empty() const;

    std::vector<std::shared_ptr<ModelCatalog>> snapshot() const;
    std::vector<CatalogDescriptor> descriptors() const;

    // One swap, so a reader never sees a catalog set mid-rebuild.
    void replace_with(std::vector<std::shared_ptr<ModelCatalog>> catalogs);

  private:
    // Equal ranks keep insertion order.
    static bool before(const std::shared_ptr<ModelCatalog>& left,
                       const std::shared_ptr<ModelCatalog>& right);

    void order_locked();

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ModelCatalog>> entries_;
};

}  // namespace models
}  // namespace lemon
