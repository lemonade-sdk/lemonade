#pragma once

#include <string>
#include <shared_mutex>
#include <functional>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Callback invoked after config changes are applied (outside the lock).
// Receives the set of keys that changed.
using ConfigSideEffectCallback = std::function<void(const std::vector<std::string>& changed_keys)>;

class RuntimeConfig {
public:
    /// Construct from a full nested config JSON (loaded from config.json).
    explicit RuntimeConfig(const json& config);

    // --- Thread-safe typed getters (shared lock) ---
    // Top-level server settings
    int port() const;
    std::string host() const;
    int websocket_port() const;
    std::string log_level() const;
    std::string extra_models_dir() const;
    bool no_broadcast() const;
    long global_timeout() const;
    int max_loaded_models() const;
    std::string models_dir() const;
    int ctx_size() const;

    // Feature flags
    bool offline() const;
    bool disable_model_filtering() const;
    bool enable_dgpu_gtt() const;

    // Backend settings (nested)
    json backend_config(const std::string& backend_name) const;
    std::string backend_string(const std::string& backend, const std::string& key) const;
    bool backend_bool(const std::string& backend, const std::string& key) const;
    int backend_int(const std::string& backend, const std::string& key) const;
    double backend_double(const std::string& backend, const std::string& key) const;

    /// Returns recipe options in the flat format that RecipeOptions/backends expect.
    /// Maps nested config to flat keys: llamacpp.backend -> llamacpp_backend,
    /// sdcpp.steps -> steps, etc.
    json recipe_options() const;

    // --- Unified setter ---
    // Validates and applies changes, then calls side_effect_cb (outside lock)
    // with the list of keys that actually changed.
    // Accepts nested JSON: {"llamacpp": {"backend": "vulkan"}} merges into
    // the llamacpp section rather than replacing it.
    // Returns JSON: {"status":"success","updated":{...}} or throws on validation error.
    json set(const json& changes, ConfigSideEffectCallback side_effect_cb = nullptr);

    // --- Full snapshot ---
    json snapshot() const;

    /// Set/get the global RuntimeConfig instance.
    /// Set once at startup; read from anywhere that needs config values.
    static void set_global(RuntimeConfig* instance);
    static RuntimeConfig* global();

    // Log format string — shared between main.cpp and apply_config_side_effects
    static constexpr const char* LOG_FORMAT =
        "%Y-%m-%d %H:%M:%S.#ms [#severity] (#tag_func) #message";

    // --- Public helpers ---

    /// Map config section name to recipe name (e.g. "sdcpp" -> "sd-cpp").
    static std::string config_section_to_recipe(const std::string& config_section);

    /// Map recipe name to config section name (e.g. "sd-cpp" -> "sdcpp").
    static std::string recipe_to_config_section(const std::string& recipe);

    /// Validate that `value` is a valid backend choice for the given config section.
    /// Must be "auto" or a backend supported on this system (via SystemInfo).
    /// Throws std::invalid_argument if invalid.
    static void validate_backend_choice(const std::string& config_section,
                                        const std::string& value);

    /// Validate that a *_bin config value is "builtin" or an existing file path.
    /// Throws std::invalid_argument if the path does not exist.
    static void validate_bin_path(const std::string& config_section,
                                  const std::string& key,
                                  const std::string& value);

private:
    // Validate a single key/value pair. Throws std::invalid_argument on failure.
    void validate(const std::string& key, const json& value) const;

    // Validate a nested backend key/value pair.
    void validate_backend(const std::string& backend, const std::string& key,
                          const json& value) const;

    // Collect changed keys (including nested paths like "llamacpp") into changed_keys.
    void apply_changes(const json& changes, std::vector<std::string>& changed_keys);

    mutable std::shared_mutex mutex_;

    // Config stored as nested JSON matching config.json structure.
    json config_;

    // Valid log levels
    static const std::vector<std::string> valid_log_levels_;
};

} // namespace lemon
