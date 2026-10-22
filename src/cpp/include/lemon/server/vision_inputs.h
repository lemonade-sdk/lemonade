#pragma once

#include <string>

#include <httplib.h>

#include "lemon/model_types.h"
#include "lemon/server/api_route.h"

namespace lemon {

// Shared by the vision routes (images/detections, images/poses, images/hands).
extern const RouteError kVisionImageInvalid;

// Replaces the `image` argument with an array of bare base64 payloads. Writes an
// OpenAI error and returns false when an input is unusable.
bool normalize_vision_images(RouteRequest& req, httplib::Response& res);

// Sets req.model to the only loaded model of `type`, for a request that names
// none. Writes an OpenAI error with `no_sole_model` and returns false when there
// is not exactly one.
bool resolve_sole_vision_model(ServerContext& ctx, RouteRequest& req, httplib::Response& res,
                               ModelType type, const RouteError& no_sole_model);

} // namespace lemon
