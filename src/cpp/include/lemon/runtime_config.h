#pragma once

#include <string>
#include <shared_mutex>
#include <functional>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Callback invoked after config changes are applied (outside the lock).
// Receives a nested JSON object containing only the entries that actually
// changed (mirrors the input shape, e.g. {"port": 9000, "llamacpp": {"vulkan_bin": "latest"}}).
using ConfigSideEffectCallback = std::function<void(const json& applied_changes)>;

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
    bool auto_evict() const;
    double auto_evict_threshold_pct() const;
    bool inhibit_suspend() const;

    // Telemetry settings
    bool telemetry_enabled() const;
    bool telemetry_hide_inputs() const;
    bool telemetry_hide_outputs() const;
    bool telemetry_hide_thinking() const;
    bool telemetry_trust_incoming_trace_context() const;
    int telemetry_max_queue_capacity() const;
    int telemetry_max_attribute_length() const;
    std::string telemetry_otlp_endpoint() const;
    std::string telemetry_otlp_protocol() const;
    std::vector<std::string> telemetry_otlp_semantics() const;
    std::map<std::string, std::string> telemetry_otlp_headers() const;
    int telemetry_otlp_max_retries() const;
    double telemetry_otlp_retry_backoff_base_s() const;
    int telemetry_otlp_send_batch_size() const;
    double telemetry_otlp_batch_timeout_s() const;


    // Feature flags
    bool offline() const;
    bool auto_check_model_updates() const;
    bool no_fetch_executables() const;
    bool disable_model_filtering() const;
    bool enable_dgpu_gtt() const;
    std::string rocm_channel() const;
    std::string rocm_channel_for_recipe(const std::string& recipe) const;

    // Backend settings (nested)
    json backend_config(const std::string& backend_name) const;
    std::string backend_string(const std::string& backend, const std::string& key) const;
    bool backend_bool(const std::string& backend, const std::string& key) const;
    int backend_int(const std::string& backend, const std::string& key) const;
    double backend_double(const std::string& backend, const std::string& key) const;

    /// Returns recipe options in the flat format that RecipeOptions/backends expect.
    /// Maps nested config to flat keys: llamacpp.backend -> llamacpp_backend,
    /// sdcpp.steps -> steps, etc.
    json recipe_options(const std::string& backend) const;

    // --- Unified setter ---
    // Validates and applies changes, then calls side_effect_cb (outside lock)
    // with a nested JSON of entries that actually changed.
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

    /// Validate a *_bin config value. Accepts:
    ///   - "" or "builtin" — use the lemonade-pinned version
    ///   - "latest"        — resolve to most-recent upstream release at lemond start
    ///   - "b8664" / "v1.8.2" / etc. — a specific upstream release tag (verbatim)
    ///   - "/path/to/bin"  — an existing pre-downloaded binary directory
    /// Throws std::invalid_argument when the value looks like a path but the
    /// path does not exist.
    static void validate_bin_path(const std::string& config_section,
                                  const std::string& key,
                                  const std::string& value);

private:
    // Validate a single key/value pair. Throws std::invalid_argument on failure.
    void validate(const std::string& key, const json& value) const;

    // Validate a nested backend key/value pair.
    void validate_backend(const std::string& backend, const std::string& key,
                          const json& value) const;

    // Apply changes and emit the subset that actually differed from the previous
    // state into `applied_diff`. Mirrors the shape of `changes` (nested for
    // backend sections).
    void apply_changes(const json& changes, json& applied_diff);

    // Helpers to retrieve configuration options with environment variable override
    // and fallback to config JSON under a shared lock.
    bool get_bool_opt(const char* env_name, const std::vector<std::string>& path, bool default_val) const;
    int get_int_opt(const char* env_name, const std::vector<std::string>& path, int default_val) const;
    double get_double_opt(const char* env_name, const std::vector<std::string>& path, double default_val) const;
    std::string get_string_opt(const char* env_name, const std::vector<std::string>& path, const std::string& default_val) const;

    mutable std::shared_mutex mutex_;

    // Config stored as nested JSON matching config.json structure.
    json config_;

    // Valid log levels
    static const std::vector<std::string> valid_log_levels_;
};

} // namespace lemon
