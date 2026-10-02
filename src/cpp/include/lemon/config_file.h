#pragma once

#include <shared_mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// ============================================================================
// Config migration helpers (static inline for testability)
// ============================================================================

/// Extract the config_version from a JSON object. Returns 0 if the field
/// is missing or not an integer — treating the config as "very old".
static inline int config_get_version(const json& config) {
    if (config.contains("config_version") && config["config_version"].is_number_integer()) {
        return config["config_version"].get<int>();
    }
    return 0;
}

/// Migrate a v1 config to v2:
///   - Upgrade ctx_size 4096 -> -1 (only if user never changed it)
///   - Bump config_version to 2
/// Returns true if the config was modified.
static inline bool config_migrate_v1_to_v2(json& config) {
    bool changed = false;

    // 1. Upgrade ctx_size 4096 -> -1 only when it is exactly the old default.
    if (config.contains("ctx_size") && config["ctx_size"].is_number_integer()) {
        int ctx_val = config["ctx_size"].get<int>();
        if (ctx_val == 4096) {
            config["ctx_size"] = -1;
            changed = true;
        }
    }

    // 2. Bump the version marker.
    config["config_version"] = 2;
    changed = true;

    return changed;
}

/// Apply in-memory migrations from an older config_version.
/// The `config` parameter is the post-merge object (defaults + user overrides);
/// `original_version` is the version from the file before merging.
/// Returns true if the config was modified.
static inline bool config_migrate(json& config,
                                  const json& defaults,
                                  int original_version = -1) {
    // Determine effective version: use the original loaded version if known,
    // otherwise fall back to reading from the config itself (handles fresh
    // configs that were just created with defaults).
    int current_version = (original_version >= 0) ? original_version
                                                  : config_get_version(config);
    int target_version = config_get_version(defaults);

    if (current_version >= target_version) {
        return false;  // Already up to date
    }

    // Apply migrations incrementally.
    if (current_version < 1) {
        // v0 -> v1 is a no-op (all fields were added by the merge).
        current_version = 1;
    }

    if (current_version < 2) {
        config_migrate_v1_to_v2(config);
        current_version = 2;
    }

    return true;
}

/// Migrate deprecated LEMONADE_ALLOWED_ORIGINS environment variable into config.json.
/// If env_origins is non-empty:
///   - If config does not contain "allowed_origins" or it is an empty string, set config["allowed_origins"] to env_origins.
///   - If config already has a non-empty "allowed_origins", do not modify it.
/// Returns true if the config was modified.
static inline bool config_migrate_allowed_origins_env(json& config, const char* env_origins) {
    if (!env_origins || *env_origins == '\0') {
        return false;
    }
    if (!config.contains("allowed_origins") ||
        (config["allowed_origins"].is_string() && config["allowed_origins"].get<std::string>().empty())) {
        config["allowed_origins"] = std::string(env_origins);
        return true;
    }
    return false;
}

// ============================================================================

/// Manages reading and writing config.json in the lemonade config dir.
class ConfigFile {
public:
    /// The canonical default config: resources/defaults.json (global keys) with
    /// each backend's per-recipe section seeded from its descriptor. Host- and
    /// deployment-independent.
    static json base_defaults();

    /// base_defaults() plus deployment overrides. On Linux, an optional distro
    /// override at /usr/share/lemonade/defaults.json (and LEMONADE_DEFAULTS_PATH)
    /// is merged on top when present.
    static json get_defaults();

    /// Load raw config.json from config_dir without merging defaults.
    /// Returns an empty JSON object if the file does not exist or is empty.
    static json load_raw(const std::string& config_dir);

    /// Load config.json from config_dir (or derived from cache_dir), deep-merging with defaults.
    /// Applies migrations for older config formats, then persists if any
    /// changes were made. Unknown keys are preserved (forward compatibility).
    static json load(const std::string& cache_dir, const std::string& config_dir = "");

    /// Save config to <config_dir>/config.json atomically (write temp, rename).
    /// Thread-safe.
    static void save(const std::string& config_dir, const json& config);

private:
    static std::shared_mutex file_mutex_;
};

} // namespace lemon
