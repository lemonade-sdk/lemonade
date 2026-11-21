#pragma once

// Backend downloads come from GitHub, or from the lemonade-sdk/backends ModelScope
// dataset, which keeps GitHub's file names (synced by tools/modelscope_mirror).

#include <stdexcept>
#include <string>
#include <utility>

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

// Thread-local, so a request's source applies only to the installs it starts.
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

/** Runs fn under the calling thread's request source, on whichever thread calls it. */
template <typename Fn>
auto carry_to_worker(Fn fn) {
    return [source = request_override, fn = std::move(fn)](auto&&... args) mutable {
        ScopedOverride scope(source);
        return fn(std::forward<decltype(args)>(args)...);
    };
}

// The mirror is a third party: only take files backend_versions.json can verify.
inline std::string require_pinned_hash(const std::string& source, std::string expected_hash,
                                       const std::string& file) {
    if (expected_hash.empty() && is_modelscope(source)) {
        throw std::runtime_error(
            "No SHA-256 for " + file + " is pinned in backend_versions.json, so it will not be "
            "installed from the ModelScope mirror. Use backend_source=github for this install, "
            "or a Lemonade release whose pins include it.");
    }
    return expected_hash;
}

inline std::string modelscope_base(const std::string& endpoint) {
    std::string base = endpoint.empty() ? std::string(kModelScopeDefaultEndpoint) : endpoint;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

inline std::string modelscope_file_url(const std::string& endpoint,
                                       const std::string& path_in_dataset) {
    return modelscope_base(endpoint) + "/api/v1/datasets/" + kModelScopeDataset +
           "/repo?Revision=master&FilePath=" + utils::url_encode(path_in_dataset);
}

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

inline std::string release_manifest_url(const std::string& endpoint,
                                        const std::string& repo,
                                        const std::string& tag) {
    return modelscope_file_url(endpoint, "github/" + repo + "/" + tag + "/assets.json");
}

inline std::string therock_tarball_url(const std::string& source,
                                       const std::string& endpoint,
                                       const std::string& version,
                                       const std::string& filename) {
    if (is_modelscope(source)) {
        return modelscope_file_url(endpoint, "therock/" + version + "/" + filename);
    }
    return std::string(kTheRockBaseUrl) + filename;
}

inline std::string reachability_probe_url(const std::string& source, const std::string& endpoint) {
    if (is_modelscope(source)) {
        return modelscope_base(endpoint) + "/";
    }
    return "https://github.com/";
}

}  // namespace lemon::backend_source
