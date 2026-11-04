#pragma once

#include <string>

#include <httplib.h>

#include "lemon/server/api_route.h"

namespace lemon {

class Router;

extern const RouteError kNotLlm;

// Writes kNotLlm, naming operation in the message, and returns false when model is not an LLM.
bool require_llm(Router& router, const std::string& model, const std::string& operation,
                 httplib::Response& res);

} // namespace lemon
