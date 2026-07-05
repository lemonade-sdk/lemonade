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
        if (r.name.empty() || r.base_url.empty()) continue;
        installed_.push_back(std::move(r));
    }
}

json CloudProviderRegistry::to_config_array() const {
    std::shared_lock lock(mu_);
    json arr = json::array();
    for (const auto& r : installed_) {
        arr.push_back({
            {"name", r.name},
            {"base_url", r.base_url},
            {"allow_insecure_http", r.allow_insecure_http}
        });
    }
    return arr;
}

bool CloudProviderRegistry::install(const std::string& provider,
                                    const std::string& base_url,
                                    bool allow_insecure_http) {
    std::unique_lock lock(mu_);
    std::string normalized = normalize_base_url(base_url);
    for (auto& r : installed_) {
        if (r.name == provider) {
            if (r.base_url == normalized &&
                r.allow_insecure_http == allow_insecure_http) {
                return false;
            }
            r.base_url = normalized;
            r.allow_insecure_http = allow_insecure_http;
            return true;
        }
    }
    installed_.push_back({provider, normalized, allow_insecure_http});
    return true;
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
