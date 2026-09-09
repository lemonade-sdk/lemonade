#pragma once

#include <mutex>
#include <optional>
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
        std::string name;                         // e.g. "fireworks", "openai"
        std::string base_url;                     // normalized: no trailing slash
        bool allow_insecure_http = false;         // explicit opt-in for http:// + API key
        // Some gateways front an OpenAI-shaped API but authenticate with a
        // differently-named header and no "Bearer " prefix. Defaults reproduce
        // the previously hardcoded Authorization/Bearer behavior.
        std::string auth_header_name = "Authorization";
        std::string auth_header_prefix = "Bearer ";
        // Request/response shape this provider speaks. "openai" (the default)
        // is what CloudServer implements; "anthropic" providers are relayed
        // from lemond's /v1/messages instead.
        std::string wire_format = "openai";

        bool operator==(const Record& other) const {
            return name == other.name && base_url == other.base_url &&
                   allow_insecure_http == other.allow_insecure_http &&
                   auth_header_name == other.auth_header_name &&
                   auth_header_prefix == other.auth_header_prefix &&
                   wire_format == other.wire_format;
        }
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

    // Everything install() can set on a record. An unset field means "leave
    // this alone": callers that only know about base_url (the desktop app's
    // add-provider form, a re-install that doesn't repeat the auth flags) must
    // not clobber settings they never asked about.
    struct InstallOptions {
        std::optional<bool> allow_insecure_http;
        std::optional<std::string> auth_header_name;
        std::optional<std::string> auth_header_prefix;
        std::optional<std::string> wire_format;
    };

    // Idempotent. Adds the provider if absent, updates base_url if present.
    // Normalizes base_url (trims trailing slash). Unset options keep the
    // existing record's value, or the Record default for a new record.
    // Returns true if the stored record changed, false if it was already
    // identical. Invalid auth header values are rejected by the caller-facing
    // validators below; passing one here stores it unchecked.
    bool install(const std::string& provider,
                 const std::string& base_url,
                 const InstallOptions& options = {});

    // Opts an already-installed provider into plaintext-HTTP key transmission
    // without disturbing any other field. Returns false if not installed.
    bool set_allow_insecure_http(const std::string& provider, bool allow);

    // Removes the provider record AND its runtime key. Returns true if a
    // record was removed.
    bool uninstall(const std::string& provider);

    bool is_installed(const std::string& provider) const;

    // Returns a copy of all installed records.
    std::vector<Record> list_installed() const;

    // Base URL for a registered provider, or empty if not installed.
    std::string base_url_for(const std::string& provider) const;

    // Whether this provider has explicit opt-in to send API keys to an
    // http:// base URL. Irrelevant for https:// providers.
    bool allow_insecure_http_for(const std::string& provider) const;

    // A provider that isn't installed yields the Record defaults rather than
    // an error, so callers can send a request without a prior existence check.
    struct AuthHeader {
        std::string name = "Authorization";
        std::string prefix = "Bearer ";
    };
    AuthHeader auth_header_for(const std::string& provider) const;

    // Wire format for a provider. Returns "openai" for a provider that isn't
    // installed, matching the default for records that predate this field.
    std::string wire_format_for(const std::string& provider) const;

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

    // Validates a candidate auth header name: a non-empty RFC 7230 token.
    // Rejecting separators and CTLs (notably CR/LF) is what keeps a
    // configured value from injecting extra header lines into every forwarded
    // request, and rejecting empty keeps a malformed ": Bearer <key>" line —
    // which sends the request out effectively unauthenticated — off the wire.
    // Returns empty string on OK, a human-readable error message otherwise.
    static std::string validate_auth_header_name(const std::string& name);

    // Empty is valid and meaningful here: gateways that expect the bare key.
    static std::string validate_auth_header_prefix(const std::string& prefix);

    // Validates a candidate base URL: must be https:// or http://. Plain HTTP
    // is allowed because custom backends can legitimately live on a trusted LAN,
    // but callers should surface base_url_warnings() so the user understands
    // the transport tradeoff.
    // Returns empty string on OK, a human-readable error message otherwise.
    static std::string validate_base_url(const std::string& base_url);

    // Only "openai" and "anthropic" are recognized; anything else would leave
    // the provider undispatchable.
    static std::string validate_wire_format(const std::string& wire_format);

    // Returns true for an http:// base URL, false for https:// or invalid input.
    static bool is_http_base_url(const std::string& base_url);

    // Non-blocking warnings for a validated base URL. When api_key_available is
    // true, includes a stronger warning that Lemonade will send Bearer auth over
    // plaintext HTTP.
    static std::vector<std::string> base_url_warnings(const std::string& base_url,
                                                      bool api_key_available);

private:
    static std::string normalize_base_url(std::string url);

    mutable std::shared_mutex mu_;
    std::vector<Record> installed_;
    std::unordered_map<std::string, std::string> runtime_keys_;
};

} // namespace lemon
