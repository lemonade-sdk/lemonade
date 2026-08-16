#include "lemon/utils/origin_utils.h"

#include <algorithm>
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

    auto scheme_end = origin.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::string scheme = origin.substr(0, scheme_end);

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

    // Configured allowed origins (for non-loopback web-app access, e.g.,
    // http://192.168.1.50:13305 when bound to --host 0.0.0.0).
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();
}

} // namespace lemon::utils
