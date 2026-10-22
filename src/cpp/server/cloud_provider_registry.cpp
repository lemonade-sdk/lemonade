#include "lemon/cloud_provider_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

namespace {

std::string to_lower_copy(const std::string& value) {
    std::string lower = value;
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower;
}

bool starts_with_scheme(const std::string& value, const std::string& scheme) {
    return to_lower_copy(value).compare(0, scheme.size(), scheme) == 0;
}

// RFC 7230 tchar.
bool is_token_char(unsigned char c) {
    return std::isalnum(c) != 0 ||
           std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string::npos;
}

CloudProviderRegistry::Record
apply_options(CloudProviderRegistry::Record record,
              const CloudProviderRegistry::InstallOptions& options) {
    if (options.allow_insecure_http) record.allow_insecure_http = *options.allow_insecure_http;
    if (options.auth_header_name) record.auth_header_name = *options.auth_header_name;
    if (options.auth_header_prefix) record.auth_header_prefix = *options.auth_header_prefix;
    if (options.wire_format) record.wire_format = *options.wire_format;
    return record;
}

} // namespace

std::string CloudProviderRegistry::env_var_name(const std::string& provider) {
    std::string upper = provider;
    for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return "LEMONADE_" + upper + "_API_KEY";
}

std::string CloudProviderRegistry::validate_provider_name(const std::string& provider) {
    if (provider.empty()) {
        return "Provider name is required";
    }
    if (provider.size() > 64) {
        return "Provider name must be 64 characters or fewer";
    }
    for (unsigned char c : provider) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) {
            return "Provider name must match [a-z0-9_-]+ (lowercase, no spaces, slashes, or dots)";
        }
    }
    return "";
}

std::string CloudProviderRegistry::validate_auth_header_name(const std::string& name) {
    if (name.empty()) {
        return "Auth header name must not be empty";
    }
    if (name.size() > 128) {
        return "Auth header name must be 128 characters or fewer";
    }
    if (!std::all_of(name.begin(), name.end(), is_token_char)) {
        return "Auth header name must be a valid HTTP header name "
               "(letters, digits, and !#$%&'*+-.^_`|~)";
    }
    return "";
}

std::string CloudProviderRegistry::validate_auth_header_prefix(const std::string& prefix) {
    if (prefix.size() > 128) {
        return "Auth header prefix must be 128 characters or fewer";
    }
    // Space and horizontal tab are the only CTL-adjacent characters a header
    // value may carry, and "Bearer " needs the space.
    const bool ok = std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) {
        return c == '\t' || (c >= 0x20 && c != 0x7F);
    });
    if (!ok) {
        return "Auth header prefix must not contain control characters "
               "(including carriage return or line feed)";
    }
    return "";
}

std::string CloudProviderRegistry::validate_base_url(const std::string& base_url) {
    if (base_url.empty()) {
        return "Base URL is required";
    }
    if (std::any_of(base_url.begin(), base_url.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        })) {
        return "Base URL must not contain whitespace";
    }
    const std::string https = "https://";
    const std::string http = "http://";
    if (starts_with_scheme(base_url, https) ||
        starts_with_scheme(base_url, http)) {
        const size_t host_start = base_url.find("://") + 3;
        const size_t host_end = base_url.find_first_of("/?#", host_start);
        if (host_end == host_start || host_start >= base_url.size()) {
            return "Base URL must include a host";
        }
        return "";
    }
    return "Base URL must start with https:// or http://";
}

std::string CloudProviderRegistry::validate_wire_format(const std::string& wire_format) {
    if (wire_format == "openai" || wire_format == "anthropic") {
        return "";
    }
    return "Wire format must be one of: openai, anthropic";
}

bool CloudProviderRegistry::is_http_base_url(const std::string& base_url) {
    return starts_with_scheme(base_url, "http://");
}

std::vector<std::string>
CloudProviderRegistry::base_url_warnings(const std::string& base_url,
                                         bool api_key_available) {
    std::vector<std::string> warnings;
    if (!is_http_base_url(base_url)) {
        return warnings;
    }
    warnings.push_back(
        "Base URL uses http://; traffic to this provider is not encrypted.");
    if (api_key_available) {
        warnings.push_back(
            "An API key is configured for this http:// provider; Lemonade will "
            "send it as a Bearer token over plaintext HTTP.");
    }
    return warnings;
}

std::string CloudProviderRegistry::normalize_base_url(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

void CloudProviderRegistry::load_from_config(const json& cloud_providers_array) {
    std::unique_lock lock(mu_);
    installed_.clear();
    if (!cloud_providers_array.is_array()) {
        return;
    }
    for (const auto& entry : cloud_providers_array) {
        if (!entry.is_object()) continue;
        if (!entry.contains("name") || !entry["name"].is_string()) continue;
        if (!entry.contains("base_url") || !entry["base_url"].is_string()) continue;
        Record r;
        r.name = entry["name"].get<std::string>();
        r.base_url = normalize_base_url(entry["base_url"].get<std::string>());
        if (entry.contains("allow_insecure_http") && entry["allow_insecure_http"].is_boolean()) {
            r.allow_insecure_http = entry["allow_insecure_http"].get<bool>();
        }
        // A value that fails validation falls back to the default rather than
        // disabling the provider: config.json is hand-editable, and a typo'd
        // header should not silently put a malformed line on the wire.
        if (entry.contains("auth_header_name") && entry["auth_header_name"].is_string()) {
            auto name = entry["auth_header_name"].get<std::string>();
            if (validate_auth_header_name(name).empty()) {
                r.auth_header_name = std::move(name);
            }
        }
        if (entry.contains("auth_header_prefix") && entry["auth_header_prefix"].is_string()) {
            auto prefix = entry["auth_header_prefix"].get<std::string>();
            if (validate_auth_header_prefix(prefix).empty()) {
                r.auth_header_prefix = std::move(prefix);
            }
        }
        // An unrecognized persisted value would leave the provider
        // undispatchable, so fall back to the default rather than store it.
        if (entry.contains("wire_format") && entry["wire_format"].is_string() &&
            validate_wire_format(entry["wire_format"].get<std::string>()).empty()) {
            r.wire_format = entry["wire_format"].get<std::string>();
        }
        if (r.name.empty() || r.base_url.empty()) continue;
        installed_.push_back(std::move(r));
    }
}

json CloudProviderRegistry::to_config_array() const {
    std::shared_lock lock(mu_);
    json arr = json::array();
    for (const auto& r : installed_) {
        json entry = {
            {"name", r.name},
            {"base_url", r.base_url},
            {"allow_insecure_http", r.allow_insecure_http}
        };
        // Written only when non-default so an existing config.json is left
        // byte-identical by a save that changed nothing.
        const Record defaults;
        if (r.auth_header_name != defaults.auth_header_name) {
            entry["auth_header_name"] = r.auth_header_name;
        }
        if (r.auth_header_prefix != defaults.auth_header_prefix) {
            entry["auth_header_prefix"] = r.auth_header_prefix;
        }
        if (r.wire_format != defaults.wire_format) {
            entry["wire_format"] = r.wire_format;
        }
        arr.push_back(std::move(entry));
    }
    return arr;
}

bool CloudProviderRegistry::install(const std::string& provider,
                                    const std::string& base_url,
                                    const InstallOptions& options) {
    std::unique_lock lock(mu_);
    auto it = std::find_if(installed_.begin(), installed_.end(),
                           [&](const Record& r) { return r.name == provider; });
    if (it == installed_.end()) {
        installed_.push_back(apply_options(Record{provider, normalize_base_url(base_url)},
                                           options));
        return true;
    }
    Record updated = apply_options(*it, options);
    updated.base_url = normalize_base_url(base_url);
    if (updated == *it) {
        return false;
    }
    *it = std::move(updated);
    return true;
}

bool CloudProviderRegistry::set_allow_insecure_http(const std::string& provider,
                                                    bool allow) {
    std::unique_lock lock(mu_);
    for (auto& r : installed_) {
        if (r.name == provider) {
            r.allow_insecure_http = allow;
            return true;
        }
    }
    return false;
}

bool CloudProviderRegistry::uninstall(const std::string& provider) {
    std::unique_lock lock(mu_);
    auto it = std::find_if(installed_.begin(), installed_.end(),
                           [&](const Record& r) { return r.name == provider; });
    if (it == installed_.end()) return false;
    installed_.erase(it);
    runtime_keys_.erase(provider);
    return true;
}

bool CloudProviderRegistry::is_installed(const std::string& provider) const {
    std::shared_lock lock(mu_);
    return std::any_of(installed_.begin(), installed_.end(),
                       [&](const Record& r) { return r.name == provider; });
}

std::vector<CloudProviderRegistry::Record>
CloudProviderRegistry::list_installed() const {
    std::shared_lock lock(mu_);
    return installed_;
}

std::string CloudProviderRegistry::base_url_for(const std::string& provider) const {
    std::shared_lock lock(mu_);
    for (const auto& r : installed_) {
        if (r.name == provider) return r.base_url;
    }
    return "";
}

bool CloudProviderRegistry::allow_insecure_http_for(const std::string& provider) const {
    std::shared_lock lock(mu_);
    for (const auto& r : installed_) {
        if (r.name == provider) return r.allow_insecure_http;
    }
    return false;
}

CloudProviderRegistry::AuthHeader
CloudProviderRegistry::auth_header_for(const std::string& provider) const {
    std::shared_lock lock(mu_);
    for (const auto& r : installed_) {
        if (r.name == provider) return {r.auth_header_name, r.auth_header_prefix};
    }
    return {};
}

std::string CloudProviderRegistry::wire_format_for(const std::string& provider) const {
    std::shared_lock lock(mu_);
    for (const auto& r : installed_) {
        if (r.name == provider) return r.wire_format;
    }
    return "openai";
}

std::string CloudProviderRegistry::resolve_key(const std::string& provider) const {
    // Env var is checked WITHOUT holding the registry lock so we don't pin the
    // shared lock across libc calls; std::getenv reads process-global state
    // that isn't ours to guard anyway.
    const std::string env_name = env_var_name(provider);
    if (const char* v = std::getenv(env_name.c_str()); v && *v) {
        return v;
    }
    std::shared_lock lock(mu_);
    auto it = runtime_keys_.find(provider);
    if (it != runtime_keys_.end()) return it->second;
    return "";
}

bool CloudProviderRegistry::set_runtime_key(const std::string& provider,
                                            const std::string& key) {
    // Env-wins-over-runtime: refuse silently with a false return so callers
    // can surface a 409 to the client. Checked without the registry lock for
    // the same reason as resolve_key.
    const std::string env_name = env_var_name(provider);
    if (const char* v = std::getenv(env_name.c_str()); v && *v) {
        return false;
    }
    std::unique_lock lock(mu_);
    if (key.empty()) {
        runtime_keys_.erase(provider);
    } else {
        runtime_keys_[provider] = key;
    }
    return true;
}

bool CloudProviderRegistry::clear_runtime_key(const std::string& provider) {
    std::unique_lock lock(mu_);
    return runtime_keys_.erase(provider) > 0;
}

CloudProviderRegistry::AuthState
CloudProviderRegistry::auth_state(const std::string& provider) const {
    AuthState s;
    const std::string env_name = env_var_name(provider);
    if (const char* v = std::getenv(env_name.c_str()); v && *v) {
        s.env_var_set = true;
    }
    std::shared_lock lock(mu_);
    s.runtime_key_set = runtime_keys_.count(provider) > 0;
    return s;
}

} // namespace lemon
