#pragma once

#include <lemon/models/catalog_set.h>
#include <lemon/models/store_builder.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;

namespace models {

class ModelArtifactStore;
class ModelCatalog;
class ModelIngest;

// What a compose produced: the set to publish, why the rest of the registry is not on
// this machine, the catalogs that produced it, and how many rows have bytes.
struct CompositionResult {
    std::map<std::string, ModelInfo> models;
    std::shared_ptr<const FilterReasons> reasons;
    std::vector<std::shared_ptr<ModelCatalog>> catalogs;
    int downloaded = 0;
};

// Per-backend metadata for a row about to be published: context windows, GGUF
// architecture. A route to someone else's GPU has no local bytes to read, and a model
// that is not here has nothing to read at all.
void populate_model_metadata(ModelInfo& info, const ModelArtifactStore& artifacts);

// Turns catalogs into a publishable set. Reading rows is a catalog's job; what is left
// is the four things a row cannot answer about itself — how its options layer, whether
// this machine can run it, whether its bytes are here, and what its routing policy
// resolves to. The order matters: routing is parsed over the filtered, downloaded set,
// while the aliases a policy resolves against must come from before the filter, or a
// component that exists but cannot run here reads as a typo (#2748).
class ModelComposition {
public:
    struct Deps {
        // Fills `built` with the catalogs to read, product's own first. The
        // `recipe_options` each row authored come back through `authored`, so no
        // document is read twice.
        std::function<void(std::map<std::string, nlohmann::json>& authored, CatalogSet& built)>
            load_catalogs;
        // What the user saved, one snapshot for the whole pass: a concurrent options
        // write cannot then split the build across two states.
        std::function<nlohmann::json()> saved_options;
        // Who reads this machine; null asks the probe.
        AvailabilityProbe availability;
        // Rows are complete only once their bytes are accounted for.
        ModelIngest* fetch;
        ModelArtifactStore* artifacts;
    };

    explicit ModelComposition(Deps deps) : deps_(std::move(deps)) {}

    CompositionResult run();

  private:
    Deps deps_;
};

}  // namespace models
}  // namespace lemon
