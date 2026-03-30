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
    RuntimeConfig(int port,
                  const std::string& host,
                  int websocket_port,
                  const std::string& log_level,
                  const std::string& extra_models_dir,
                  bool no_broadcast,
                  long global_timeout,
                  int max_loaded_models,
                  const json& recipe_options);

    // --- Thread-safe typed getters (shared lock) ---
    int port() const;
    std::string host() const;
    int websocket_port() const;
    std::string log_level() const;
    std::string extra_models_dir() const;
    bool no_broadcast() const;
    long global_timeout() const;
    int max_loaded_models() const;
    json recipe_options() const;

    // --- Unified setter ---
    // Validates and applies changes, then calls side_effect_cb (outside lock)
    // with the list of keys that actually changed.
    // Returns JSON: {"status":"success","updated":{...}} or throws on validation error.
    json set(const json& changes, ConfigSideEffectCallback side_effect_cb = nullptr);

    // --- Full snapshot ---
    json snapshot() const;

    // Log format string — shared between main.cpp and apply_config_side_effects
    static constexpr const char* LOG_FORMAT =
        "%Y-%m-%d %H:%M:%S.#ms [#severity] (#tag_func) #message";

private:
    // Validate a single key/value pair. Throws std::invalid_argument on failure.
    void validate(const std::string& key, const json& value) const;

    mutable std::shared_mutex mutex_;

    // All config lives in a single flat JSON object.
    // Server keys (port, host, ...) and recipe keys (ctx_size, ...) are peers.
    // Typed getters provide compile-time-safe access; set()/snapshot() are generic.
    json config_;

    // Valid log levels
    static const std::vector<std::string> valid_log_levels_;
};

} // namespace lemon
