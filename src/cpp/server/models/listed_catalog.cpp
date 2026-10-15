#include <lemon/models/listed_catalog.h>

#include <utility>

namespace lemon {
namespace models {

ListedCatalog::ListedCatalog(std::string provider, List list)
    : descriptor_{
          "listed:" + provider, false,
          DocEntry{"listed-" + provider, "Cloud: " + provider,
                   "A cloud provider's own model listing.",
                   {"A provider that will not answer costs only its own entries."}, ""},
          ""},
      list_(std::move(list)) {}

void ListedCatalog::enumerate(CatalogAccumulator& accumulator) const {
    std::vector<ModelInfo> listed;
    try {
        listed = list_();
    } catch (...) {
        // One provider that will not answer must not take the model set down.
        return;
    }

    for (auto& info : listed) {
        accumulator.add(CatalogEntry{descriptor_.id, std::move(info)});
    }
}

}  // namespace models
}  // namespace lemon
