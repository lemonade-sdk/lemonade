#pragma once

#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_arg.h"
#include "lemon/server/route_error.h"

namespace lemon {

struct ServerContext;

namespace video_routes {
extern const RouteArg kId;
} // namespace video_routes

extern const RouteError kUnknownVideo;
extern const RouteError kVideoNotActive;
extern const RouteError kVideoNotReady;
extern const RouteError kVideoNoOutput;

std::optional<nlohmann::json> find_video(ServerContext& ctx, const std::string& id);

std::string video_status(const std::string& job_status);

void write_unknown_video(httplib::Response& res);

} // namespace lemon
