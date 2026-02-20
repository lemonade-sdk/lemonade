#pragma once

#include <string>
#include <functional>
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
    bool is_installed(const std::string& recipe, const std::string& backend);
    std::string get_installed_version(const std::string& recipe, const std::string& backend);
    std::string get_latest_version(const std::string& recipe, const std::string& backend);

    // List all recipes with their backends and install status
    json get_all_backends_status();

    // Get GitHub release URL for a recipe/backend
    std::string get_release_url(const std::string& recipe, const std::string& backend);

    // Get the platform-specific download filename for a recipe/backend (empty if N/A)
    std::string get_download_filename(const std::string& recipe, const std::string& backend);

private:
    // Installation parameters for a backend
    struct InstallParams {
        std::string repo;
        std::string filename;
        std::string version;
    };

    // Get the install parameters for a recipe/backend combination
    InstallParams get_install_params(const std::string& recipe, const std::string& backend);

    // Get install params for each recipe type
    InstallParams get_llamacpp_params(const std::string& backend, const std::string& version);
    InstallParams get_whispercpp_params(const std::string& backend, const std::string& version);
    InstallParams get_sdcpp_params(const std::string& backend, const std::string& version);
    InstallParams get_kokoro_params(const std::string& backend, const std::string& version);
    InstallParams get_ryzenai_params(const std::string& backend, const std::string& version);

    // FLM has special install logic (uses installer exe)
    void install_flm(const std::string& backend);
};

} // namespace lemon
