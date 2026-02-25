#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include "model_manager.h"  // For DownloadProgressCallback

namespace lemon {

using json = nlohmann::json;

class BackendManager {
public:
    BackendManager();

    // Core operations
    void install_backend(const std::string& recipe, const std::string& backend,
                         DownloadProgressCallback progress_cb = nullptr);
    void uninstall_backend(const std::string& recipe, const std::string& backend);

    // Query operations
    std::string get_latest_version(const std::string& recipe, const std::string& backend);

    // List all recipes with their backends and install status
    json get_all_backends_status();

    // Get GitHub release URL for a recipe/backend
    std::string get_release_url(const std::string& recipe, const std::string& backend);

    // Get the platform-specific download filename for a recipe/backend (empty if N/A)
    std::string get_download_filename(const std::string& recipe, const std::string& backend);

    // Enrichment data for a backend (all fields computed in a single call)
    struct BackendEnrichment {
        std::string release_url;
        std::string download_filename;
        std::string version;
    };

    // Get all enrichment data for a backend in one call (avoids repeated config lookups)
    BackendEnrichment get_backend_enrichment(const std::string& recipe, const std::string& backend);

    // Recipes cache: populated by Server on first system-info request,
    // then kept up-to-date by install_backend/uninstall_backend with targeted updates.
    void set_recipes_cache(const json& recipes);
    json get_recipes_cache();

private:
    // Update a single backend entry in the cached recipes JSON
    void update_recipes_cache_entry(const std::string& recipe, const std::string& backend, bool installed);

    json cached_recipes_;
    std::mutex cache_mutex_;
    // Cached backend_versions.json (loaded once at construction)
    json backend_versions_;

    // Get version for a recipe/backend from the cached config
    std::string get_version_from_config(const std::string& recipe, const std::string& backend);

    // Install parameters for a backend (repo + filename + version)
    struct InstallParams {
        std::string repo;
        std::string filename;
        std::string version;
    };

    // Get the install parameters for a recipe/backend combination
    InstallParams get_install_params(const std::string& recipe, const std::string& backend);

};

} // namespace lemon
