#include "lemon/utils/container_registry.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "lemon/utils/url_utils.h"

namespace lemon {
namespace utils {

namespace {

constexpr long kRegistryTimeoutSeconds = 30;

// Multi-arch images answer with an index, single-arch ones with a manifest;
// the digest of whichever the tag names is what a pull by digest fetches.
const char* const kManifestTypes =
    "application/vnd.oci.image.index.v1+json, "
    "application/vnd.docker.distribution.manifest.list.v2+json, "
    "application/vnd.oci.image.manifest.v1+json, "
    "application/vnd.docker.distribution.manifest.v2+json";

// One parameter of a `Bearer realm="...",service="...",scope="..."` challenge.
std::string challenge_param(const std::string& challenge, const std::string& key) {
    const std::string needle = key + "=\"";
    for (size_t pos = challenge.find(needle); pos != std::string::npos;
         pos = challenge.find(needle, pos + needle.size())) {
        if (pos > 0 && challenge[pos - 1] != ' ' && challenge[pos - 1] != ',') continue;
        const size_t start = pos + needle.size();
        const size_t end = challenge.find('"', start);
        return end == std::string::npos ? std::string() : challenge.substr(start, end - start);
    }
    return "";
}

std::string header(const HttpResponse& response, const std::string& name) {
    const auto it = response.headers.find(name);
    return it == response.headers.end() ? std::string() : it->second;
}

bool is_digest(const std::string& value) {
    const std::string prefix = "sha256:";
    return value.size() == prefix.size() + 64 && value.compare(0, prefix.size(), prefix) == 0 &&
           std::all_of(value.begin() + prefix.size(), value.end(), [](unsigned char c) {
               return std::isdigit(c) || (c >= 'a' && c <= 'f');
           });
}

}  // namespace

std::string resolve_registry_digest(const std::string& repository, const std::string& tag,
                                    const RegistryRequest& request_override) {
    const RegistryRequest request = request_override
        ? request_override
        : [](const std::string& method, const std::string& url,
             const std::map<std::string, std::string>& headers) {
              return method == "HEAD" ? HttpClient::head(url, headers, kRegistryTimeoutSeconds)
                                      : HttpClient::get(url, headers, kRegistryTimeoutSeconds);
          };

    const auto slash = repository.find('/');
    if (slash == std::string::npos) {
        throw std::invalid_argument("'" + repository + "' does not name a registry");
    }
    std::string host = repository.substr(0, slash);
    if (host == "docker.io") host = "registry-1.docker.io";
    const std::string url =
        "https://" + host + "/v2/" + repository.substr(slash + 1) + "/manifests/" + tag;

    std::map<std::string, std::string> headers = {{"Accept", kManifestTypes}};
    // Docker Hub counts a manifest GET against its anonymous pull limit; HEAD
    // carries the same digest header without counting.
    HttpResponse response = request("HEAD", url, headers);
    if (response.status_code == 401) {
        const std::string challenge = header(response, "www-authenticate");
        const std::string realm = challenge_param(challenge, "realm");
        if (realm.empty()) {
            throw std::runtime_error(repository + "'s registry asks for credentials Lemonade does not have");
        }
        const HttpResponse token_response =
            request("GET", realm + "?service=" + url_encode(challenge_param(challenge, "service")) +
                    "&scope=" + url_encode(challenge_param(challenge, "scope")),
                {});
        std::string token;
        if (token_response.status_code == 200) {
            const auto body = nlohmann::json::parse(token_response.body, nullptr, false);
            if (body.is_object()) {
                token = body.value("token", body.value("access_token", ""));
            }
        }
        if (token.empty()) {
            throw std::runtime_error("Could not get a pull token for " + repository + " (HTTP " +
                                     std::to_string(token_response.status_code) + ")");
        }
        headers["Authorization"] = "Bearer " + token;
        response = request("HEAD", url, headers);
    }
    if (response.status_code == 404) {
        throw std::runtime_error(repository + " has no tag '" + tag + "'");
    }
    if (response.status_code != 200) {
        throw std::runtime_error("Could not look up " + repository + ":" + tag + " (HTTP " +
                                 std::to_string(response.status_code) + ")");
    }
    const std::string digest = header(response, "docker-content-digest");
    if (!is_digest(digest)) {
        throw std::runtime_error(repository + "'s registry gave no digest for '" + tag + "'");
    }
    return digest;
}

}  // namespace utils
}  // namespace lemon
