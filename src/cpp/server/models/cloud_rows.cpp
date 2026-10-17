#include <lemon/models/cloud_rows.h>

#include <lemon/model_manager.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <utility>

namespace lemon {
namespace models {

namespace {
// The recipe a provider's listing reports, and the only one these rows own.
constexpr const char* kCloudRecipe = "cloud";
}  // namespace

CloudRows::CloudRows(ModelStore& store, List list) : store_(store), list_(std::move(list)) {}

std::size_t CloudRows::refresh(const std::string& provider) {
    if (provider.empty() || !list_) {
        return 0;
    }

    // The same listing the catalogs enumerate, so a refresh and a build cannot disagree
    // about what a provider has. Runs outside the writer lock because a slow provider can
    // take 15 s.
    const std::vector<ModelInfo> models = list_(provider);

    ModelStore::Write write(store_);

    // Reseed: drop this provider's previously-registered entries before
    // inserting the fresh list, so a model the provider stopped exposing
    // disappears. Other providers' entries are untouched.
    for (auto it = write.draft().begin(); it != write.draft().end();) {
        if (it->second.recipe == kCloudRecipe && it->second.cloud_provider == provider) {
            it = write.draft().erase(it);
        } else {
            ++it;
        }
    }

    size_t added = 0;
    for (const auto& m : models) {
        if (m.recipe != kCloudRecipe || m.model_name.empty()) continue;
        // Match a build's precedence exactly: emplace, don't overwrite.
        // Any pre-existing entry under the same bare cache key wins - whether
        // it's an FLM model, another cloud provider that discovered first, or
        // a builtin/extra/user record. This provider's previously-registered
        // entries are already cleared above, so emplace here is symmetric
        // with a build and immune to a fast /cloud/auth racing past an
        // already-populated cache.
        ModelInfo info = m;
        // The listing populates name/checkpoint/labels/context/cost but
        // not recipe_options; Router needs it to construct CloudServer.
        info.recipe_options = RecipeOptions(kCloudRecipe, json::object());
        auto [it, inserted] = write.draft().emplace(info.model_name, std::move(info));
        if (!inserted) {
            LOG(INFO, "CloudRows")
                << "Cloud discovery for '" << provider << "' skipping '"
                << it->first << "': name already held (recipe="
                << it->second.recipe << ")" << std::endl;
            continue;
        }
        ++added;
    }

    LOG(INFO, "CloudRows") << "Refreshed cloud models for provider '"
                               << provider << "': " << added << " model(s)"
                               << std::endl;
    return added;
}

std::size_t CloudRows::evict(const std::string& provider) {
    if (provider.empty()) return 0;
    ModelStore::Write write(store_);
    size_t removed = 0;
    for (auto it = write.draft().begin(); it != write.draft().end();) {
        if (it->second.recipe == kCloudRecipe && it->second.cloud_provider == provider) {
            it = write.draft().erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG(DEBUG, "CloudRows") << "Evicted " << removed
                                    << " cloud model(s) for provider '"
                                    << provider << "'" << std::endl;
    }
    return removed;
}

std::size_t CloudRows::count(const std::string& provider) const {
    auto snapshot = store_.read();
    return std::count_if(snapshot->models.begin(), snapshot->models.end(),
                         [&](const auto& kv) {
                             return kv.second.recipe == kCloudRecipe &&
                                    kv.second.cloud_provider == provider;
                         });
}

}  // namespace models
}  // namespace lemon
