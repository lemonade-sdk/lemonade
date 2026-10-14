#include <lemon/models/catalog.h>

namespace lemon {
namespace models {

CatalogAccumulator::Outcome CatalogAccumulator::add(CatalogEntry entry, Conflict conflict) {
    const std::string id = entry.info.model_name;
    auto existing = models_.find(id);
    if (existing == models_.end()) {
        models_.emplace(id, std::move(entry.info));
        return Outcome::Added;
    }
    if (conflict == Conflict::Overwrite) {
        existing->second = std::move(entry.info);
        return Outcome::Added;
    }
    return Outcome::KeptExisting;
}

void EnumeratedCatalog::enumerate(CatalogAccumulator& accumulator) const {
    std::vector<ModelInfo> discovered;
    try {
        discovered = discover_(accumulator);
    } catch (...) {
        if (descriptor_.required) {
            throw;
        }
        return;
    }

    for (auto& info : discovered) {
        const std::string name = info.model_name;
        if (accumulator.add(CatalogEntry{descriptor_.id, std::move(info)}) ==
                CatalogAccumulator::Outcome::KeptExisting &&
            on_conflict_) {
            on_conflict_(name);
        }
    }
}

}  // namespace models
}  // namespace lemon
