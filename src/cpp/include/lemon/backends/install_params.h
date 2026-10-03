#pragma once

#include <string>

namespace lemon::backends {

struct InstallParams {
    std::string repo;
    std::string filename;
    std::string version_override;
    // An exact asset URL bypasses GitHub discovery and release URL construction.
    std::string download_url;
};

inline std::string release_asset_url(const std::string& repo,
                                     const std::string& version,
                                     const std::string& filename,
                                     const std::string& download_url) {
    if (!download_url.empty()) return download_url;
    return "https://github.com/" + repo + "/releases/download/" + version + "/" + filename;
}

inline std::string release_page_url(const std::string& repo,
                                    const std::string& version,
                                    const std::string& download_url) {
    if (!download_url.empty()) return download_url;
    return "https://github.com/" + repo + "/releases/tag/" + version;
}

} // namespace lemon::backends
