#pragma once

// Where pinned backend release files are downloaded from (`backend_source`,
// env LEMONADE_BACKEND_SOURCE): GitHub Releases, or the lemonade-sdk/backends
// dataset on ModelScope for networks that cannot reach GitHub reliably.
// The dataset keeps each file's GitHub name:
//   github/<owner>/<repo>/<tag>/<file>   plus assets.json listing that release
//   therock/<version>/<file>             ROCm runtime tarballs
// tools/modelscope_mirror/modelscope_mirror.py keeps it in sync with
// backend_versions.json. MODELSCOPE_ENDPOINT moves the API, as for model pulls.

#include <string>

#include <lemon/utils/url_utils.h>

namespace lemon::backend_source {

inline constexpr const char* kGitHub = "github";
inline constexpr const char* kModelScope = "modelscope";
inline constexpr const char* kModelScopeDataset = "lemonade-sdk/backends";
inline constexpr const char* kModelScopeDefaultEndpoint = "https://modelscope.cn";
inline constexpr const char* kTheRockBaseUrl = "https://stable.repo.amd.com/rocm/core/tarball/";

inline bool is_modelscope(const std::string& source) {
    return source == kModelScope;
}

inline bool is_valid(const std::string& source) {
    return source == kGitHub || source == kModelScope;
}

// A request's backend source (install, load and their CLI flags) beats the
// config for the installs that request starts. Thread-local because the install runs on the
// thread that opens this scope, which may be a download-manager worker.
inline thread_local std::string request_override;

class ScopedOverride {
public:
    explicit ScopedOverride(const std::string& source) : previous_(request_override) {
        if (!source.empty()) {
            request_override = source;
        }
    }
    ~ScopedOverride() { request_override = previous_; }
    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;

private:
    std::string previous_;
};

inline std::string modelscope_base(const std::string& endpoint) {
    std::string base = endpoint.empty() ? std::string(kModelScopeDefaultEndpoint) : endpoint;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

/** Direct download URL for one file in the ModelScope dataset. */
inline std::string modelscope_file_url(const std::string& endpoint,
                                       const std::string& path_in_dataset) {
    return modelscope_base(endpoint) + "/api/v1/datasets/" + kModelScopeDataset +
           "/repo?Revision=master&FilePath=" + utils::url_encode(path_in_dataset);
}

/** URL of one GitHub release asset, or of its mirrored copy. */
inline std::string release_asset_url(const std::string& source,
                                     const std::string& endpoint,
                                     const std::string& repo,
                                     const std::string& tag,
                                     const std::string& filename) {
    if (is_modelscope(source)) {
        return modelscope_file_url(endpoint, "github/" + repo + "/" + tag + "/" + filename);
    }
    return "https://github.com/" + repo + "/releases/download/" + tag + "/" + filename;
}

/** assets.json for one mirrored release (ModelScope only). */
inline std::string release_manifest_url(const std::string& endpoint,
                                        const std::string& repo,
                                        const std::string& tag) {
    return modelscope_file_url(endpoint, "github/" + repo + "/" + tag + "/assets.json");
}

/** URL of a TheRock ROCm runtime tarball, or of its mirrored copy. */
inline std::string therock_tarball_url(const std::string& source,
                                       const std::string& endpoint,
                                       const std::string& version,
                                       const std::string& filename) {
    if (is_modelscope(source)) {
        return modelscope_file_url(endpoint, "therock/" + version + "/" + filename);
    }
    return std::string(kTheRockBaseUrl) + filename;
}

/** Host probed to decide whether backend updates can be fetched right now. */
inline std::string reachability_probe_url(const std::string& source, const std::string& endpoint) {
    if (is_modelscope(source)) {
        return modelscope_base(endpoint) + "/";
    }
    return "https://github.com/";
}

}  // namespace lemon::backend_source
