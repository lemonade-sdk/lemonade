#include "lemon/cloud_provider_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

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
    const std::string lower = [&]() {
        std::string s = base_url;
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    const std::string https = "https://";
    const std::string http = "http://";
    if (lower.compare(0, https.size(), https) == 0) {
        return "";
    }
    if (lower.compare(0, http.size(), http) == 0) {
        // Allow only loopback hosts so the Bearer API key isn't sent in
        // plaintext on a typo'd scheme. Mock-provider tests use these.
        const std::string host_and_rest = lower.substr(http.size());
        auto host_end = host_and_rest.find_first_of(":/");
        const std::string host = host_end == std::string::npos
                                     ? host_and_rest
                                     : host_and_rest.substr(0, host_end);
        if (host == "localhost" || host == "127.0.0.1" || host == "[::1]") {
            return "";
        }
        return "Base URL uses http:// to a non-loopback host (" + host +
               "); refused because it would send the Bearer API key in plaintext. "
               "Use https:// or set the host to localhost / 127.0.0.1 for local mocks.";
    }
    return "Base URL must start with https:// (or http:// for localhost)";
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
        if (r.name.empty() || r.base_url.empty()) continue;
        installed_.push_back(std::move(r));
    }
}

json CloudProviderRegistry::to_config_array() const {
    std::shared_lock lock(mu_);
    json arr = json::array();
    for (const auto& r : installed_) {
        arr.push_back({{"name", r.name}, {"base_url", r.base_url}});
    }
    return arr;
}

bool CloudProviderRegistry::install(const std::string& provider,
                                    const std::string& base_url) {
    std::unique_lock lock(mu_);
    std::string normalized = normalize_base_url(base_url);
    for (auto& r : installed_) {
        if (r.name == provider) {
            if (r.base_url == normalized) return false;
            r.base_url = normalized;
            return true;
        }
    }
    installed_.push_back({provider, normalized});
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
