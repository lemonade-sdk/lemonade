#pragma once

#include "catalog.h"

#include <functional>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// A cloud provider's model listing. The listing arrives through a callable, so
// the store holds no include of the provider layer.
class ListedCatalog : public ModelCatalog {
  public:
    using List = std::function<std::vector<ModelInfo>()>;

    ListedCatalog(std::string provider, List list);

    const CatalogDescriptor& descriptor() const override { return descriptor_; }

    void enumerate(CatalogAccumulator& accumulator) const override;

  private:
    CatalogDescriptor descriptor_;
    List list_;
};

}  // namespace models
}  // namespace lemon
