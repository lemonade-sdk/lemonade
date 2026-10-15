#pragma once

#include <lemon/models/catalog.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// The catalogs a store builds from. A build enumerates them in declared rank
// order, so a catalog that joins later takes its precedence from its descriptor
// instead of from where its construction happened to land.
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
