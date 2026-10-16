#pragma once

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_error.h"

namespace lemon {

// Install and uninstall serve cloud providers too, so `recipe` is required only for a local
// backend.
extern const RouteError kMissingRecipe;

// Reads `recipe` and `backend` from body. Returns false after writing kMissingRecipe when
// `recipe` is missing.
bool read_local_backend(const nlohmann::json& body, std::string& recipe, std::string& backend,
                        httplib::Response& res);

} // namespace lemon
