#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/api_route.h"

namespace lemon {

// Naming an Omni collection in chat/completions runs a server-side tool-calling loop
// across the collection's components instead of one completion.
bool is_omni_collection(ServerContext& ctx, const std::string& model);

// Loads every component, then answers the request with the orchestrated completion.
void run_omni_collection(ServerContext& ctx, RouteRequest& req, httplib::Response& res);

} // namespace lemon
