#pragma once

#include <functional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/router.h"
#include "lemon/server/route_error.h"

namespace lemon {

// Slots and tokenize act on the most recently used model's llama.cpp server rather than on a
// named model, so they reach it the same way and fail the same way.
class RecentLlamaCpp {
public:
    // Writes call()'s response, or the error when no model is loaded, Router finds none
    // once it stops waiting, or the model is not llama.cpp. operation names the request in
    // the no-model message.
    static void serve(Router& router, httplib::Response& res, const std::string& operation,
                      const std::function<nlohmann::json()>& call);

    static const RouteError kNoModelLoaded;
    static const RouteError kNotLlamaCpp;
    static std::vector<RouteError> errors();
};

} // namespace lemon
