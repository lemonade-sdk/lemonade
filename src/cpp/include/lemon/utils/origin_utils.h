#pragma once

#include "lemon/runtime_config.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

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

    if (to_lower(str) == "null") {
        out.host = "null";
        return out;
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
        if (!path_part.empty() && (out.scheme == "http" || out.scheme == "https" || out.scheme == "ws" || out.scheme == "wss")) {
            return Origin{};
        }
        host_and_port = host_and_port.substr(0, slash_pos);
    }

    if (host_and_port.empty()) {
        if (!out.scheme.empty()) {
            out.host = out.scheme;
            return out;
        }
        return Origin{};
    }

    if (host_and_port[0] == '[') {
        size_t bracket_end = host_and_port.find(']');
        if (bracket_end == std::string::npos) {
            return Origin{};
        }
        std::string raw_ipv6 = host_and_port.substr(1, bracket_end - 1);
        size_t zone_pos = raw_ipv6.find('%');
        if (zone_pos != std::string::npos) {
            raw_ipv6 = raw_ipv6.substr(0, zone_pos);
        }
        out.host = "[" + to_lower(raw_ipv6) + "]";
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

    if (out.host.size() > 1 && out.host.back() == '.' && out.host.front() != '[') {
        out.host.pop_back();
    }

    return out;
}

inline bool is_loopback_origin(const Origin& origin) {
    if (origin.host == "localhost" || origin.host == "127.0.0.1" || origin.host == "[::1]" || origin.host == "::1" || origin.host == "tauri.localhost") {
        return true;
    }

    if (!origin.scheme.empty() &&
        origin.scheme != "http" && origin.scheme != "https" &&
        origin.scheme != "ws" && origin.scheme != "wss") {
        return true;
    }

    return false;
}

inline std::unordered_set<std::string> enumerate_server_self_set(const std::string& bound_host = "") {
    std::unordered_set<std::string> self_set;

    self_set.insert("localhost");
    self_set.insert("127.0.0.1");
    self_set.insert("::1");
    self_set.insert("[::1]");

    if (!bound_host.empty() && bound_host != "0.0.0.0" && bound_host != "::") {
        std::string h = to_lower(bound_host);
        if (h.size() > 1 && h.back() == '.' && h.front() != '[') {
            h.pop_back();
        }
        self_set.insert(h);
        if (h.front() != '[' && h.find(':') != std::string::npos) {
            self_set.insert("[" + h + "]");
        }
    }

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        std::string h = to_lower(hostname);
        if (h.size() > 1 && h.back() == '.') {
            h.pop_back();
        }
        if (!h.empty()) {
            self_set.insert(h);
            if (h.find('.') == std::string::npos) {
                self_set.insert(h + ".local");
            }
        }
    }

#ifdef _WIN32
    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES adapters = nullptr;
    ULONG ret = 0;
    do {
        adapters = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!adapters) break;
        ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            free(adapters);
            adapters = nullptr;
        }
    } while (ret == ERROR_BUFFER_OVERFLOW);

    if (ret == NO_ERROR && adapters) {
        for (auto adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr) continue;
                if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                    auto sa = (struct sockaddr_in*)unicast->Address.lpSockaddr;
                    char ip[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
                        self_set.insert(to_lower(ip));
                    }
                } else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
                    auto sa = (struct sockaddr_in6*)unicast->Address.lpSockaddr;
                    char ip[INET6_ADDRSTRLEN];
                    if (inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof(ip))) {
                        std::string ip_str = to_lower(ip);
                        self_set.insert(ip_str);
                        self_set.insert("[" + ip_str + "]");
                    }
                }
            }
        }
    }
    if (adapters) free(adapters);
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
        for (auto ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN];
                auto sa = (struct sockaddr_in*)ifa->ifa_addr;
                if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
                    self_set.insert(to_lower(ip));
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                char ip[INET6_ADDRSTRLEN];
                auto sa = (struct sockaddr_in6*)ifa->ifa_addr;
                if (inet_ntop(AF_INET6, &sa->sin6_addr, ip, sizeof(ip))) {
                    std::string ip_str = to_lower(ip);
                    self_set.insert(ip_str);
                    self_set.insert("[" + ip_str + "]");
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif

    return self_set;
}

inline std::unordered_set<std::string> get_server_self_set(const std::string& bound_host = "", bool force_refresh = false) {
    static std::mutex cache_mutex;
    static std::unordered_set<std::string> cached_set;
    static std::string cached_bound_host;
    static std::chrono::steady_clock::time_point last_refresh;
    static constexpr std::chrono::seconds kCacheTtl{60};

    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (force_refresh || cached_set.empty() || bound_host != cached_bound_host || (now - last_refresh) >= kCacheTtl) {
        cached_set = enumerate_server_self_set(bound_host);
        cached_bound_host = bound_host;
        last_refresh = now;
    }
    return cached_set;
}

inline bool is_same_origin(
    const std::string& origin_str,
    const std::string& host_header,
    const std::string& scheme = "http",
    const std::string& bound_host = "",
    const std::unordered_set<std::string>& self_set = {}) {

    if (origin_str.empty() || host_header.empty()) {
        return false;
    }
    Origin request_origin = parse_origin(origin_str);
    if (!request_origin.is_valid() || request_origin.scheme.empty()) {
        return false;
    }
    Origin host_origin = parse_origin(host_header);
    if (!host_origin.is_valid()) {
        return false;
    }
    host_origin.scheme = to_lower(scheme.empty() ? "http" : scheme);

    if (!request_origin.matches(host_origin)) {
        return false;
    }

    if (is_loopback_origin(host_origin)) {
        return true;
    }

    const std::unordered_set<std::string>& effective_self_set =
        !self_set.empty() ? self_set : get_server_self_set(bound_host);

    if (effective_self_set.find(host_origin.host) != effective_self_set.end()) {
        return true;
    }

    return false;
}

inline bool is_same_origin(
    const std::string& origin_str,
    const std::string& host_header,
    const std::string& scheme,
    const std::unordered_set<std::string>& self_set) {
    return is_same_origin(origin_str, host_header, scheme, "", self_set);
}

inline std::string resolve_allowed_origins() {
    if (auto* cfg = RuntimeConfig::global()) {
        return cfg->allowed_origins();
    }
    const char* env_origins = std::getenv("LEMONADE_ALLOWED_ORIGINS");
    return env_origins ? std::string(env_origins) : "";
}

inline bool is_origin_allowed(
    const std::string& origin_str,
    const std::string& allowed_origins,
    const std::string& host_header = "",
    const std::string& scheme = "http",
    const std::string& bound_host = "",
    const std::unordered_set<std::string>& self_set = {}) {

    // Non-browser HTTP clients (curl, CLI, SDKs) send no Origin header and are allowed.
    if (origin_str.empty()) {
        return true;
    }
    Origin request_origin = parse_origin(origin_str);
    if (!request_origin.is_valid()) {
        return false;
    }

    // Layer 1: Loopback and native desktop application schemes (localhost, 127.0.0.1, [::1], *.localhost, lemonade://, file://, app://, jan://, vscode-webview://)
    if (is_loopback_origin(request_origin)) {
        return true;
    }

    // Layer 2: Explicit allowed_origins set (authoritative for non-loopback origins)
    if (!allowed_origins.empty()) {
        if (allowed_origins == "*") {
            return true;
        }

        std::stringstream ss(allowed_origins);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item.erase(0, item.find_first_not_of(" \t\r\n"));
            if (!item.empty()) {
                item.erase(item.find_last_not_of(" \t\r\n") + 1);
            }
            const bool is_opaque_null = request_origin.scheme.empty() && request_origin.host == "null" && request_origin.port == -1;
            if (to_lower(item) == "null" && is_opaque_null) {
                return true;
            }
            Origin allowed_origin = parse_origin(item);
            if (allowed_origin.is_valid() && !allowed_origin.scheme.empty() && request_origin.matches(allowed_origin)) {
                return true;
            }
        }
        // When explicit allowlist is configured, it is authoritative — no fallback to same-origin.
        return false;
    }

    // Layer 3: Same-origin validated against server_self_set from OS (zero-config LAN, HTTP + WS)
    if (!host_header.empty() && is_same_origin(origin_str, host_header, scheme, bound_host, self_set)) {
        return true;
    }

    // Layer 4: Otherwise reject
    return false;
}

inline bool is_origin_allowed(
    const std::string& origin_str,
    const std::string& allowed_origins,
    const std::string& host_header,
    const std::string& scheme,
    const std::unordered_set<std::string>& self_set) {
    return is_origin_allowed(origin_str, allowed_origins, host_header, scheme, "", self_set);
}

inline bool is_websocket_origin_allowed(
    const std::string& origin_str,
    const std::string& allowed_origins,
    const std::string& host_header = "",
    const std::string& scheme = "http",
    const std::string& bound_host = "",
    const std::unordered_set<std::string>& self_set = {}) {

    return is_origin_allowed(origin_str, allowed_origins, host_header, scheme, bound_host, self_set);
}

inline bool is_websocket_origin_allowed(
    const std::string& origin_str,
    const std::string& allowed_origins,
    const std::string& host_header,
    const std::string& scheme,
    const std::unordered_set<std::string>& self_set) {

    return is_websocket_origin_allowed(origin_str, allowed_origins, host_header, scheme, "", self_set);
}

} // namespace lemon::utils
