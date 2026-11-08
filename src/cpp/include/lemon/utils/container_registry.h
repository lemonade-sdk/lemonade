#pragma once

#include <functional>
#include <map>
#include <string>

#include "lemon/utils/http_client.h"

namespace lemon {
namespace utils {

using RegistryGet = std::function<HttpResponse(const std::string& url,
                                               const std::map<std::string, std::string>& headers)>;

// The digest `tag` currently names in `repository`'s registry, such as
// "sha256:<64 hex digits>" for "docker.io/kyuz0/amd-strix-halo-toolboxes" and
// "rocm-10.0-gufo". Asks the registry's manifest endpoint, with the anonymous
// pull token its 401 answer points to, so nothing is pulled. Throws when the
// registry cannot answer. `get` defaults to HttpClient::get.
std::string resolve_registry_digest(const std::string& repository, const std::string& tag,
                                    const RegistryGet& get = {});

}  // namespace utils
}  // namespace lemon
