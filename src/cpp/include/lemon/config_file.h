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

    /// Auto-migrate from env vars and /etc/lemonade/lemonade.conf if config.json
    /// doesn't exist. Creates config.json with migrated values merged over defaults.
    /// Logs a notice about what was migrated.
    static void migrate(const std::string& home_dir);

private:
    /// Read a KEY=VALUE style conf file and return env var mappings found.
    static json read_conf_file(const std::string& path);

    /// Map a LEMONADE_* env var name to its config.json path.
    /// Returns a pair of (top-level key, nested key or "").
    /// Returns ("", "") for unknown/unmapped env vars.
    static std::pair<std::string, std::string> env_to_config_key(const std::string& env_name);

    /// Get platform-specific default models directory.
    static std::string default_models_dir();

    static std::shared_mutex file_mutex_;
};

} // namespace lemon
