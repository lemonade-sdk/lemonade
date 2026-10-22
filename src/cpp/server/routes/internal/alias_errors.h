#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/route_error.h"

namespace lemon {

// Alias failures are not model-lookup failures. ModelLoader::model_error() reports any
// name missing from the registry as "model not found", which a new alias always is, so
// the alias routes write this OpenAI-style body with `param` set to `alias` instead.
extern const RouteError kAliasStoreFailed;

void write_alias_error(httplib::Response& res, const RouteError& error, const std::string& message,
                       const std::string& type = "invalid_request_error");

} // namespace lemon
