#pragma once

#include <functional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/route_error.h"
#include "lemon/server/server_context.h"

namespace lemon {

using ModelOptionsMutation =
    std::function<bool(const std::string& model_key, ModelInfo& info, httplib::Response& res)>;

// The saved, effective and default options that every models/{id}/options route returns.
nlohmann::ordered_json model_options_schema();

// The three options routes resolve the model their path's id names, apply their mutation
// (none for GET), and answer with the saved, effective and default options. The
// mutation updates info to the state the response describes; returning false means
// it already wrote an error.
void respond_with_model_options(ServerContext& ctx, const std::string& id,
                                httplib::Response& res, const ModelOptionsMutation& mutation);
std::vector<RouteError> model_options_errors();

} // namespace lemon
