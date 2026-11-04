#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/route_error.h"

namespace lemon {

// Reading and deleting a job both look it up by the path's id.
extern const RouteError kUnknownJob;
void write_unknown_job(httplib::Response& res);

// Pause and interrupt both act only on a job that is queued or running.
extern const RouteError kJobNotActive;
void write_job_not_active(httplib::Response& res, const std::string& refused_action);

} // namespace lemon
