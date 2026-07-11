#pragma once

#include <string>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace lemon::utils {

struct Origin {
    std::string scheme;
    std::string host;
    int port = -1;

    bool is_valid() const {
        return !host.empty();
    }

    int get_effective_port() const {
        if (port != -1) {
            return port;
        }
        if (scheme == "http" || scheme == "ws") {
            return 80;
        }
        if (scheme == "https" || scheme == "wss") {
            return 443;
        }
        return -1;
    }

    bool matches(const Origin& pattern) const {
        if (scheme != pattern.scheme) {
            return false;
        }

        if (host != pattern.host) {
            return false;
        }

        if (get_effective_port() != pattern.get_effective_port()) {
            return false;
        }

        return true;
    }
};

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline Origin parse_origin(const std::string& origin_str) {
    Origin out;
    std::string str = origin_str;

    str.erase(0, str.find_first_not_of(" \t\r\n"));
    if (!str.empty()) {
        str.erase(str.find_last_not_of(" \t\r\n") + 1);
    }

    if (str.empty()) {
        return out;
    }

    if (str.find('@') != std::string::npos || str.find('?') != std::string::npos || str.find('#') != std::string::npos) {
        return Origin{};
    }

    size_t scheme_pos = str.find("://");
    std::string host_and_port = str;
    if (scheme_pos != std::string::npos) {
        out.scheme = to_lower(str.substr(0, scheme_pos));
        host_and_port = str.substr(scheme_pos + 3);
    }

    size_t slash_pos = host_and_port.find('/');
    if (slash_pos != std::string::npos) {
        std::string path_part = host_and_port.substr(slash_pos + 1);
        if (!path_part.empty()) {
            return Origin{};
        }
        host_and_port = host_and_port.substr(0, slash_pos);
    }

    if (host_and_port.empty()) {
        return Origin{};
    }

    if (host_and_port[0] == '[') {
        size_t bracket_end = host_and_port.find(']');
        if (bracket_end == std::string::npos) {
            return Origin{};
        }
        out.host = to_lower(host_and_port.substr(0, bracket_end + 1));
        std::string rest = host_and_port.substr(bracket_end + 1);
        if (!rest.empty()) {
            if (rest[0] == ':') {
                std::string port_str = rest.substr(1);
                if (port_str.empty() || !std::all_of(port_str.begin(), port_str.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    return Origin{};
                }
                try {
                    size_t idx = 0;
                    long long p = std::stoll(port_str, &idx);
                    if (idx != port_str.size() || p < 0 || p > 65535) {
                        return Origin{};
                    }
                    out.port = static_cast<int>(p);
                } catch (...) {
                    return Origin{};
                }
            } else {
                return Origin{};
            }
        }
    } else {
        size_t first_colon = host_and_port.find(':');
        size_t last_colon = host_and_port.find_last_of(':');
        if (last_colon != std::string::npos) {
            if (first_colon == last_colon) {
                out.host = to_lower(host_and_port.substr(0, last_colon));
                std::string port_str = host_and_port.substr(last_colon + 1);
                if (port_str.empty() || !std::all_of(port_str.begin(), port_str.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    return Origin{};
                }
                try {
                    size_t idx = 0;
                    long long p = std::stoll(port_str, &idx);
                    if (idx != port_str.size() || p < 0 || p > 65535) {
                        return Origin{};
                    }
                    out.port = static_cast<int>(p);
                } catch (...) {
                    return Origin{};
                }
            } else {
                out.host = to_lower(host_and_port);
            }
        } else {
            out.host = to_lower(host_and_port);
        }
    }

    return out;
}

inline bool is_loopback_origin(const Origin& origin) {
    return origin.host == "localhost" || origin.host == "127.0.0.1" || origin.host == "[::1]" || origin.host == "::1" || origin.host == "tauri.localhost";
}

inline bool is_origin_allowed(const std::string& origin_str, const std::string& allowed_origins_env) {
    if (origin_str.empty()) {
        return false;
    }
    Origin request_origin = parse_origin(origin_str);
    if (!request_origin.is_valid()) {
        return false;
    }

    if (is_loopback_origin(request_origin)) {
        return true;
    }

    if (allowed_origins_env.empty()) {
        return false;
    }

    if (allowed_origins_env == "*") {
        return true;
    }

    std::stringstream ss(allowed_origins_env);
    std::string item;
    while (std::getline(ss, item, ',')) {
        Origin allowed_origin = parse_origin(item);
        if (allowed_origin.is_valid() && !allowed_origin.scheme.empty() && request_origin.matches(allowed_origin)) {
            return true;
        }
    }

    return false;
}

inline bool is_websocket_origin_allowed(const std::string& origin_str, const std::string& allowed_origins_env) {
    if (origin_str.empty()) {
        return false;
    }
    Origin request_origin = parse_origin(origin_str);
    if (!request_origin.is_valid()) {
        return false;
    }

    if (is_loopback_origin(request_origin)) {
        return true;
    }

    // Note: Non-local WebSocket origins must not fall back to same-origin comparison.
    // Both Host and Origin headers are controlled by the client/browser, which allows
    // DNS-rebinding attacks to bypass origin validation. Therefore, non-local WebSocket
    // connections must explicitly match the allowed origins config.

    if (allowed_origins_env == "*") {
        return true;
    }

    if (allowed_origins_env.empty()) {
        return false;
    }

    std::stringstream ss(allowed_origins_env);
    std::string item;
    while (std::getline(ss, item, ',')) {
        Origin allowed_origin = parse_origin(item);
        if (allowed_origin.is_valid() && !allowed_origin.scheme.empty() && request_origin.matches(allowed_origin)) {
            return true;
        }
    }

    return false;
}

} // namespace lemon::utils
