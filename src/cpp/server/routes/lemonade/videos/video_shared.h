#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/route_arg.h"
#include "lemon/server/route_error.h"

namespace lemon {

namespace video_routes {
extern const RouteArg kId;
} // namespace video_routes

extern const RouteError kUnknownVideo;
extern const RouteError kVideoNotActive;
extern const RouteError kVideoNotReady;
extern const RouteError kVideoNoOutput;

std::string video_status(const std::string& job_status);

void write_unknown_video(httplib::Response& res);

} // namespace lemon
