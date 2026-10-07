#pragma once

#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/server_context.h"

namespace lemon {

// The installed-provider list survives a restart through config.json, so install,
// uninstall and cloud/auth write it back after changing it. A write failure is logged
// and swallowed: the in-memory change already happened and stays usable.
void persist_cloud_providers(ServerContext& ctx);

// An API key must not reach an http:// provider unless the client opted in, so install and
// cloud/auth refuse it with this error.
void write_insecure_http_error(httplib::Response& res, const std::string& provider);

// Adds "warnings", plus the single joined "warning" string that older clients read.
void attach_warnings(nlohmann::json& response, const std::vector<std::string>& warnings);

} // namespace lemon
