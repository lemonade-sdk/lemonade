#include <lemon/models/store_patches.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/canonical_id.h>
#include <lemon/models/model_info.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/composition.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/aixlog.hpp>

#include <string>
#include <utility>

namespace lemon {
namespace models {

void StorePatches::options(std::map<std::string, ModelInfo>& draft, const ModelInfo& info) {
    auto it = draft.find(info.model_name);
    if (it != draft.end()) {
        it->second.recipe_options = info.recipe_options;
    } else {
        LOG(WARNING, "StorePatches") << "'" << info.model_name << "' not found in cache" << std::endl;
    }
}

void StorePatches::removed(const std::string& model_name) {
    ModelStore::Write store_write(*deps_.store);

    if (!store_write.cache_valid()) {
        return;
    }

    auto it = store_write.draft().find(model_name);
    if (it != store_write.draft().end()) {
        // User models and local uploads should be removed entirely from cache
        // (they're not in server_models.json, so keeping them makes no sense)
        bool is_user_model = is_user_model_name(model_name);
        if (is_user_model || it->second.source == "local_upload") {
            store_write.draft().erase(model_name);
            LOG(INFO, "StorePatches") << "Removed '" << model_name << "' from cache" << std::endl;
        } else {
            it->second.downloaded = false;
            it->second.update_available = false;
            LOG(INFO, "StorePatches") << "Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}

void StorePatches::download_status(const std::string& model_name, bool downloaded) {
    // Everything here is filesystem work, so it runs against the published view on
    // a private copy and only the rows it changed are committed.
    auto view = deps_.store->read();
    if (!view || !view->valid) {
        return;  // Will rebuild on next access
    }
    const ModelInfo* found = view->find(model_name);
    if (!found) {
        LOG(WARNING, "StorePatches") << "'" << model_name << "' not found in cache" << std::endl;
        return;
    }

    ModelInfo patched = *found;
    patched.downloaded = downloaded;
    if (downloaded) {
        // What just arrived cannot also be behind.
        patched.update_available = false;
        deps_.artifacts->resolve_all(patched);
    } else {
        patched.max_context_window = 0;
    }
    const bool backend_lists_models =
        downloaded && backends::ops_for(patched.recipe)->invalidates_cache_after_download();
    if (!backend_lists_models) {
        populate_model_metadata(patched, *deps_.artifacts);
        ModelArtifactStore::refresh_on_disk_size(patched);
    }

    // Collections carrying this model follow it without a full rebuild.
    const auto dependents =
        collection_states_after_component(view->models, model_name, downloaded);
    const std::string resolved = patched.resolved_path();

    ModelStore::Write store_write(*deps_.store);
    if (!store_write.cache_valid()) {
        return;  // Something else rebuilt while this ran, and it read the disk again
    }
    if (auto it = store_write.draft().find(model_name); it != store_write.draft().end()) {
        // Options are patched into the live row without invalidating, so a save that
        // landed while this read the disk keeps its answer.
        patched.recipe_options = it->second.recipe_options;
        it->second = std::move(patched);
    }
    for (const auto& [name, state] : dependents) {
        auto it = store_write.draft().find(name);
        if (it == store_write.draft().end() || it->second.downloaded == state) continue;
        it->second.downloaded = state;
        LOG(INFO, "StorePatches") << "Collection '" << name << "' downloaded=" << state
                                 << " (dependent on " << model_name << ")" << std::endl;
    }
    if (backend_lists_models) {
        store_write.invalidate_cache();
        LOG(INFO, "StorePatches") << "Invalidated model cache after download for '" << model_name
                                 << "' (backend rebuilds its model list)" << std::endl;
        return;
    }
    LOG(INFO, "StorePatches") << "Updated '" << model_name << "' downloaded=" << downloaded
                             << ", resolved_path=" << resolved << std::endl;
}

}  // namespace models
}  // namespace lemon
