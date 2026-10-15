#include <lemon/models/catalog_set.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <utility>

namespace lemon {
namespace models {

std::vector<std::shared_ptr<ModelCatalog>> product_catalogs(const CatalogSetDeps& deps) {
    std::vector<std::shared_ptr<ModelCatalog>> catalogs;
    const RegistryDocuments::Pin documents =
        deps.pin_registries ? deps.pin_registries() : RegistryDocuments::Pin{};

    RegistryRowRules builtin_rows = deps.registry_rows;
    builtin_rows.source_label = "built-in model";
    catalogs.push_back(std::make_shared<JsonCatalog>(
        make_catalog_descriptor(
            "server_models.json", catalog_rank::builtin_registry, Conflict::Overwrite,
            DocEntry{"server_models", "Built-in registry",
                     "The model list shipped with the server.",
                     {"Nothing writes this file back: a new row ships in a release."}, ""}),
        documents.builtin, std::move(builtin_rows)));

    // Registrations are keyed bare on disk and "user.<name>" in the cache.
    RegistryRowRules user_rows = deps.registry_rows;
    user_rows.name_prefix = "user.";
    user_rows.suggested_default = true;
    user_rows.source_label = "user model";
    catalogs.push_back(std::make_shared<JsonCatalog>(
        make_catalog_descriptor(
            "user_models.json", catalog_rank::registrations, Conflict::Overwrite,
            DocEntry{"user_models", "Registrations",
                     "Every model the user pulled or registered.",
                     {"A registration keeps its own provenance, so a later built-in row "
                      "cannot take its name."},
                     ""}),
        documents.user, std::move(user_rows)));

    if (deps.scan_extra_models) {
        catalogs.push_back(std::make_shared<EnumeratedCatalog>(
            make_catalog_descriptor(
                "extra_models_dir", catalog_rank::scan, Conflict::KeepExisting,
                DocEntry{"extra-models", "Extra models directory",
                         "A scan of the extra models directory; GGUF files are identified "
                         "by their header, not their name.",
                         {"A directory watcher requests one rebuild when the tree changes."},
                         ""},
                "extra_models_dir"),
            [scan = deps.scan_extra_models](const CatalogAccumulator& claimed) {
                return scan(claimed);
            },
            [](const std::string& name) {
                LOG(INFO, "ModelManager") << "Warning: Discovered model '" << name
                                          << "' conflicts with another extra.* registration; skipping."
                                          << std::endl;
            }));
    }

    // One catalog per dynamic backend, and one per cloud provider rather than one
    // for all of them, so a single provider's failure costs only its own entries.
    const std::vector<std::string> recipes =
        deps.dynamic_recipes ? deps.dynamic_recipes() : std::vector<std::string>{};
    for (const std::string& recipe : recipes) {
        if (recipe == "cloud") {
            if (deps.offline) {
                LOG(DEBUG, "ModelManager")
                    << "Offline mode enabled, skipping cloud model listings" << std::endl;
                continue;
            }
            const std::vector<std::string> providers =
                deps.cloud_providers ? deps.cloud_providers() : std::vector<std::string>{};
            for (const std::string& provider : providers) {
                catalogs.push_back(std::make_shared<ListedCatalog>(
                    provider, [list = deps.list_cloud_models, provider] { return list(provider); }));
            }
            continue;
        }

        catalogs.push_back(std::make_shared<EnumeratedCatalog>(
            make_catalog_descriptor(
                "discovered:" + recipe, catalog_rank::backend_discovery, Conflict::KeepExisting,
                DocEntry{"discovered-" + recipe,
                         "Installed backend: " + recipe,
                         "Models an installed backend reports holding.",
                         {"Nothing is downloaded here: the backend already has them."}, ""},
                "", true),
            [discover = deps.discover_backend, recipe](const CatalogAccumulator&) {
                return discover(recipe);
            }));
    }

    return catalogs;
}

void CatalogSet::add(std::shared_ptr<ModelCatalog> catalog) {
    if (!catalog) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = catalog->descriptor().id;
    for (auto& held : entries_) {
        if (held->descriptor().id == id) {
            held = std::move(catalog);
            order_locked();
            return;
        }
    }
    entries_.push_back(std::move(catalog));
    order_locked();
}

bool CatalogSet::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto kept = std::remove_if(entries_.begin(), entries_.end(),
                                    [&id](const std::shared_ptr<ModelCatalog>& held) {
                                        return held->descriptor().id == id;
                                    });
    if (kept == entries_.end()) {
        return false;
    }
    entries_.erase(kept, entries_.end());
    return true;
}

std::shared_ptr<ModelCatalog> CatalogSet::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& held : entries_) {
        if (held->descriptor().id == id) {
            return held;
        }
    }
    return nullptr;
}

size_t CatalogSet::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool CatalogSet::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

std::vector<std::shared_ptr<ModelCatalog>> CatalogSet::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ModelCatalog>> catalogs;
    catalogs.reserve(entries_.size());
    for (const auto& held : entries_) {
        catalogs.push_back(held);
    }
    return catalogs;
}

std::vector<CatalogDescriptor> CatalogSet::descriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CatalogDescriptor> descriptors;
    descriptors.reserve(entries_.size());
    for (const auto& held : entries_) {
        descriptors.push_back(held->descriptor());
    }
    return descriptors;
}

void CatalogSet::replace_with(std::vector<std::shared_ptr<ModelCatalog>> catalogs) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    for (auto& catalog : catalogs) {
        if (catalog) {
            entries_.push_back(std::move(catalog));
        }
    }
    order_locked();
}

bool CatalogSet::before(const std::shared_ptr<ModelCatalog>& left,
                        const std::shared_ptr<ModelCatalog>& right) {
    return left->descriptor().rank < right->descriptor().rank;
}

void CatalogSet::order_locked() {
    std::stable_sort(entries_.begin(), entries_.end(), &CatalogSet::before);
}

}  // namespace models
}  // namespace lemon
