#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include "lemon/utils/http_client.h"

namespace lemon::utils::github_api {

// Fallback wait when GitHub reports a rate limit without usable
// Retry-After / X-RateLimit-Reset headers.
inline constexpr int64_t kDefaultRateLimitBackoffSeconds = 60;

namespace detail {

inline std::string header_value(const HttpResponse& resp, const std::string& name) {
    auto it = resp.headers.find(name);
    if (it == resp.headers.end()) {
        return "";
    }
    const std::string& raw = it->second;
    const size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = raw.find_last_not_of(" \t\r\n");
    return raw.substr(first, last - first + 1);
}

inline bool parse_int64(const std::string& s, int64_t& out) {
    if (s.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

} // namespace detail

// True when the response is a GitHub rate-limit rejection: HTTP 429, or a
// 403 carrying rate-limit headers (X-RateLimit-Remaining: 0 or Retry-After).
inline bool is_rate_limited(const HttpResponse& resp) {
    if (resp.status_code == 429) {
        return true;
    }
    if (resp.status_code != 403) {
        return false;
    }
    if (detail::header_value(resp, "x-ratelimit-remaining") == "0") {
        return true;
    }
    return !detail::header_value(resp, "retry-after").empty();
}

// How long to wait before calling the GitHub API again after `resp`, in
// seconds: Retry-After when present, else the X-RateLimit-Reset epoch minus
// `now_epoch_seconds`, else a conservative default. Returns 0 when the
// response is not a rate-limit rejection.
inline int64_t rate_limit_backoff_seconds(const HttpResponse& resp,
                                          int64_t now_epoch_seconds) {
    if (!is_rate_limited(resp)) {
        return 0;
    }
    int64_t retry_after = 0;
    if (detail::parse_int64(detail::header_value(resp, "retry-after"), retry_after)) {
        return retry_after;
    }
    int64_t reset = 0;
    if (detail::parse_int64(detail::header_value(resp, "x-ratelimit-reset"), reset) &&
        reset > now_epoch_seconds) {
        return reset - now_epoch_seconds;
    }
    return kDefaultRateLimitBackoffSeconds;
}


// Return HTTP headers for GitHub API requests. Includes an Authorization
// header when GITHUB_TOKEN or GH_TOKEN is set in the environment, raising the
// rate limit from the unauthenticated 60 requests/hour.
inline std::map<std::string, std::string> headers() {
    std::map<std::string, std::string> h = {
        {"User-Agent", "lemonade"},
        {"Accept", "application/vnd.github+json"},
    };
    const char* token = std::getenv("GITHUB_TOKEN");
    if (!token || token[0] == '\0') token = std::getenv("GH_TOKEN");
    if (token && token[0] != '\0') {
        h["Authorization"] = std::string("Bearer ") + token;
    }
    return h;
}

// GET for api.github.com with headers(). When an auth token was included and
// the response is 401/403/404 (e.g. a scoped GitHub Actions token rejected by
// an unrelated repo, which GitHub surfaces as any of these statuses), retries
// once without the Authorization header. Rate-limit rejections are exempt:
// retrying anonymously would only burn the much smaller anonymous quota.
inline HttpResponse get(
    const std::string& url,
    const std::map<std::string, std::string>& extra_headers = {}) {
    auto h = headers();
    for (const auto& [k, v] : extra_headers) {
        h[k] = v;
    }
    auto resp = HttpClient::get(url, h);
    if (h.count("Authorization") &&
        (resp.status_code == 401 ||
         resp.status_code == 403 ||
         resp.status_code == 404) &&
        !is_rate_limited(resp)) {
        h.erase("Authorization");
        resp = HttpClient::get(url, h);
    }
    return resp;
}

} // namespace lemon::utils::github_api
