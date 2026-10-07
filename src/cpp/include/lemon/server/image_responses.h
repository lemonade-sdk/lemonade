#pragma once

#include <exception>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

// The OpenAI image object that every images route returns.
nlohmann::json image_response_schema();

// The image routes answered any JSON error, wherever it was raised, as invalid JSON,
// and other failures with a 500 whose type differs by route.
void write_image_route_exception(const std::exception& error, httplib::Response& res,
                                 const char* fallback_type);

// images/edits and images/variations accept the same n and source image fields; the
// backend expects the image base64-encoded in the request body.
bool attach_form_image(RouteRequest& req, httplib::Response& res);

} // namespace lemon
