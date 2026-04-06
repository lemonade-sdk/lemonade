#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

/// Manages reading, writing, and migrating config.json in the lemonade cache dir.
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
    /// When config.json doesn't exist yet, read legacy LEMONADE_* environment
    /// variables and overlay them on top of defaults.  Returns the merged config.
    static json migrate_from_env(const json& defaults);

    static std::shared_mutex file_mutex_;
};

} // namespace lemon
