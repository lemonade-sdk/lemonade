#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace lemon {

/// Server-side registry of cloud providers.
///
/// Two stores, one persisted, one not:
///   - `installed_` (persisted to config.json under "cloud_providers"):
///     {name, base_url} records for every provider the server is configured
///     to talk to. Never contains credentials.
///   - `runtime_keys_` (process memory only, never serialized): per-provider
///     API keys supplied at runtime via POST /v1/cloud/auth.
///
/// Credential resolution priority (resolve_key):
///   1. LEMONADE_<PROVIDER_UPPER>_API_KEY env var, if set
///   2. runtime_keys_[provider], if set
///   3. empty string (caller treats as 401 / "no key")
///
/// Env wins by design: an operator who provisions a "house" key via env must
/// be able to trust that a runtime POST cannot silently override it.
class CloudProviderRegistry {
public:
    struct Record {
        std::string name;      // e.g. "fireworks", "openai"
        std::string base_url;  // normalized: no trailing slash
    };

    struct AuthState {
        bool env_var_set = false;
        bool runtime_key_set = false;
    };

    CloudProviderRegistry() = default;

    // Seed from a parsed JSON array (the value of "cloud_providers" in
    // config.json). Tolerates a missing/non-array argument — no providers
    // installed is a valid state. Caller does this once at startup.
    void load_from_config(const nlohmann::json& cloud_providers_array);

    // Serialize to a JSON array suitable for writing back into config.json's
    // "cloud_providers" field. Excludes runtime keys by construction.
    nlohmann::json to_config_array() const;

    // Idempotent. Adds the provider if absent, updates base_url if present.
    // Normalizes base_url (trims trailing slash). Returns true if the stored
    // record changed, false if it was already identical.
    bool install(const std::string& provider, const std::string& base_url);

    // Removes the provider record AND its runtime key. Returns true if a
    // record was removed.
    bool uninstall(const std::string& provider);

    bool is_installed(const std::string& provider) const;

    // Returns a copy of all installed records.
    std::vector<Record> list_installed() const;

    // Base URL for a registered provider, or empty if not installed.
    std::string base_url_for(const std::string& provider) const;

    // Resolves an API key for a provider:
    //   1. Returns the LEMONADE_<PROVIDER_UPPER>_API_KEY env var if set.
    //   2. Returns runtime_keys_[provider] if set.
    //   3. Otherwise returns empty string.
    std::string resolve_key(const std::string& provider) const;

    // Sets the in-memory runtime key. Returns:
    //   - false if the env var is set for that provider (the caller treats
    //     this as a 409 conflict; the runtime key is NOT stored, env wins).
    //   - true on success.
    // No-op-if-empty: passing an empty key is treated as a delete.
    bool set_runtime_key(const std::string& provider, const std::string& key);

    // Removes the in-memory runtime key. Returns true if one was present.
    bool clear_runtime_key(const std::string& provider);

    // Reports what kinds of auth are currently available for a provider.
    // Both flags can be true (env set AND runtime set) — in that case the
    // env var takes precedence per resolve_key.
    AuthState auth_state(const std::string& provider) const;

    // Convenience: returns the canonical env-var name for a provider
    // (e.g. "fireworks" -> "LEMONADE_FIREWORKS_API_KEY"). Public so tests
    // and CLI can render the same name in error messages.
    static std::string env_var_name(const std::string& provider);

    // Validates a candidate provider name against the registry's accepted
    // character set ([a-z0-9_-]+, non-empty). Lowercase-only is enforced
    // because env_var_name() uppercases — "Fireworks" and "fireworks" would
    // otherwise be distinct records resolving the same env var. Slashes /
    // spaces / dots also break dot-namespaced model ids. Returns empty
    // string on OK, a human-readable error message otherwise.
    static std::string validate_provider_name(const std::string& provider);

    // Validates a candidate base URL: must be https:// (any host), or
    // http:// limited to localhost / 127.0.0.1 / ::1 so the mock-provider
    // tests still work. Anything else is rejected — a typo'd scheme would
    // leak the Bearer API key in plaintext on every forwarded request.
    // Returns empty string on OK, a human-readable error message otherwise.
    static std::string validate_base_url(const std::string& base_url);

private:
    static std::string normalize_base_url(std::string url);

    mutable std::shared_mutex mu_;
    std::vector<Record> installed_;
    std::unordered_map<std::string, std::string> runtime_keys_;
};

} // namespace lemon
