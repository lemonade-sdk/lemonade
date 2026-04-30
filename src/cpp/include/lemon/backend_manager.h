#pragma once

#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "model_manager.h"  // For DownloadProgressCallback

namespace lemon {

using json = nlohmann::json;

class BackendManager {
public:
    BackendManager();

    // Core operations
    void install_backend(const std::string& recipe, const std::string& backend,
                         bool force = false,
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

    // Returns the most-recent upstream release tag for the given (recipe, backend),
    // resolved via GitHub. The result is cached for the lifetime of this
    // BackendManager so each repo is queried at most once. Returns "" on failure
    // (offline, no_fetch_executables=true, network error, parse error). No-throw
    // — used by the status path which must degrade gracefully.
    std::string get_or_resolve_latest_tag(const std::string& recipe,
                                          const std::string& backend);

    /// Set/get the global BackendManager instance. Mirrors RuntimeConfig::global()
    /// so non-Server contexts (status helpers in system_info.cpp) can reach it.
    /// Set once from Server's constructor; null until then.
    static void set_global(BackendManager* instance);
    static BackendManager* global();

private:
    // Cached backend_versions.json (loaded once at construction)
    json backend_versions_;

    // Cached "latest" GitHub release tags, keyed by repo (e.g. "ggml-org/llama.cpp").
    // Populated lazily on first resolution; never invalidated within a process.
    // Restart `lemond` to re-resolve.
    std::unordered_map<std::string, std::string> latest_version_cache_;
    std::mutex latest_version_cache_mutex_;

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

    // Resolve the user's *_bin config value for a (recipe, backend) into a
    // concrete release tag. Falls back to `pinned_version` for "" / "builtin"
    // / path-syntax values. For "latest", queries GitHub (cached). For any
    // other value, returns the value verbatim as a release tag.
    // Throws on failure for "latest" when offline/no_fetch_executables block
    // resolution and no installed version.txt exists.
    std::string resolve_user_version(const std::string& recipe,
                                     const std::string& resolved_backend,
                                     const std::string& pinned_version,
                                     const std::string& repo);

    // Fetch the "latest" release tag from GitHub for `repo`. Honors the cache.
    // `throw_on_failure=true` (default for the install path) raises an error
    // describing what went wrong. `false` (status path) returns "" silently.
    std::string fetch_latest_github_tag(const std::string& repo,
                                        bool throw_on_failure);
};

} // namespace lemon
