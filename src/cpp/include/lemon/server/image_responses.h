#pragma once

#include <exception>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

// The OpenAI image object that every images route returns.
nlohmann::ordered_json image_response_schema();

// Image route clients expect any JSON error, wherever it was raised, as invalid JSON,
// and any other failure as a 500 typed fallback_type.
void write_image_route_exception(const std::exception& error, httplib::Response& res,
                                 const char* fallback_type);
std::vector<RouteError> image_route_exception_errors();

// Passes the backend's body through as is, with kImageBackendError's status when it holds an error.
extern const RouteError kImageBackendError;
void write_image_backend_response(const nlohmann::json& response, httplib::Response& res);

// images/edits and images/variations accept the same n and source image fields; the
// backend expects each image base64-encoded in the request body. images/edits takes several
// reference images, which allow_multiple also forwards in upload order as image_files.
bool attach_form_image(RouteRequest& req, httplib::Response& res, const RouteArg& image,
                       bool allow_multiple = false);
std::vector<RouteError> attach_form_image_errors(bool allow_multiple = false);

} // namespace lemon
