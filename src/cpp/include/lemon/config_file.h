#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

/// Manages reading, writing, and migrating config.json in the lemonade home dir.
class ConfigFile {
public:
    /// Returns the full default config with nested backend sections.
    /// Platform-specific defaults (e.g. models_dir) are resolved at call time.
    static json get_defaults();

    /// Load config.json from home_dir, deep-merging with defaults.
    /// If the file doesn't exist, creates it with defaults.
    /// Unknown keys are preserved (forward compatibility).
    static json load(const std::string& home_dir);

    /// Save config to <home_dir>/config.json atomically (write temp, rename).
    /// Thread-safe.
    static void save(const std::string& home_dir, const json& config);

private:
    /// Get platform-specific default models directory.
    static std::string default_models_dir();

    static std::shared_mutex file_mutex_;
};

} // namespace lemon
