#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include "lemon/utils/http_client.h"

namespace lemon::utils::github_api {

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
// once without the Authorization header.
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
         resp.status_code == 404)) {
        h.erase("Authorization");
        resp = HttpClient::get(url, h);
    }
    return resp;
}

} // namespace lemon::utils::github_api
