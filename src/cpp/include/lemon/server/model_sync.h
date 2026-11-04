#pragma once

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_error.h"
#include "lemon/server/server_context.h"

namespace lemon {

// POST /internal/models/sync and GET /internal/models/sync/status answer with the same
// status object.
nlohmann::ordered_json model_sync_status_schema();

// Checking for model updates and syncing them both reach the registry, which offline mode
// forbids.
extern const RouteError kOfflineUpdates;

// Returns true after writing kOfflineUpdates when offline mode is on. action is the verb the
// message names, such as "check".
bool refuse_offline_updates(ServerContext& ctx, httplib::Response& res, const std::string& action);

// Pulling, listing a repository's variants and searching a registry all need the network,
// which offline mode forbids.
extern const RouteError kLemondOffline;

// Returns true after writing kLemondOffline when offline mode is on.
bool refuse_offline_download(ServerContext& ctx, httplib::Response& res);

} // namespace lemon
