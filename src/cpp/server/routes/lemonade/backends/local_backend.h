#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/route_error.h"

namespace lemon {

// Install and uninstall serve cloud providers too, so `recipe` is required only for a local
// backend.
extern const RouteError kMissingRecipe;

// Returns false after writing kMissingRecipe when recipe is empty.
bool require_local_recipe(const std::string& recipe, httplib::Response& res);

} // namespace lemon
