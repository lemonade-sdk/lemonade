#pragma once

#include <shared_mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

/// Manages reading and writing config.json in the lemonade cache dir.
class ConfigFile {
public:
    /// Returns the full default config loaded from installed resource JSON.
    /// On Linux, an optional distro override at /usr/share/lemonade/defaults.json
    /// is merged on top when present.
    static json get_defaults();

    /// Load config.json from cache_dir, deep-merging with defaults.
    /// If the file doesn't exist, creates it with defaults.
    /// Unknown keys are preserved (forward compatibility).
    static json load(const std::string& cache_dir);

    /// Save config to <cache_dir>/config.json atomically (write temp, rename).
    /// Thread-safe.
    static void save(const std::string& cache_dir, const json& config);

private:
    static std::shared_mutex file_mutex_;
};

} // namespace lemon
