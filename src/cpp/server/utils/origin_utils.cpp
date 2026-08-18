#include "lemon/utils/origin_utils.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace lemon::utils {

bool is_origin_allowed(const std::string& origin,
                       const std::vector<std::string>& allowed_origins) {
    if (origin.empty()) {
        return false;
    }

    // Native desktop-app origins (Tauri custom scheme / WebView2 virtual host).
    static const std::set<std::string> app_origins = {
        "tauri://localhost", "http://tauri.localhost", "https://tauri.localhost"
    };
    if (app_origins.count(origin)) {
        return true;
    }

    // Explicitly configured origins (the config `allowed_origins` array and the
    // LEMONADE_ALLOWED_ORIGINS env var, merged in RuntimeConfig::allowed_origins).
    // A bare "*" allows any origin; otherwise the Origin must match an entry
    // exactly. Exact matching also gates the opaque "null" origin, which is only
    // honored when "null" is explicitly allow-listed and never matches a web
    // origin whose host happens to be "null" (e.g. http://null).
    for (const auto& allowed : allowed_origins) {
        if (allowed == "*" || allowed == origin) {
            return true;
        }
    }

    auto scheme_end = origin.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    std::string scheme = origin.substr(0, scheme_end);
    // Browsers lowercase the scheme, but normalize defensively so an uppercase
    // "HTTP://" can't slip past the web-scheme checks into the allow-all branch.
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Non-web schemes (file://, app://, jan://, etc.) require local filesystem
    // access or a desktop wrapper, so a remote page can't forge them.
    if (!scheme.empty() && scheme != "http" && scheme != "https" &&
        scheme != "ws" && scheme != "wss") {
        return true;
    }
    std::string host = origin.substr(scheme_end + 3);
    if (!host.empty() && host.front() == '[') {
        // IPv6 literal: keep the bracketed host, drop any :port suffix.
        auto close = host.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = host.substr(0, close + 1);
    } else {
        auto colon = host.find(':');
        if (colon != std::string::npos) {
            host = host.substr(0, colon);
        }
    }
    if (host == "localhost" || host == "127.0.0.1" ||
        host == "[::1]" || host == "::1") {
        return true;
    }

    return false;
}

} // namespace lemon::utils
