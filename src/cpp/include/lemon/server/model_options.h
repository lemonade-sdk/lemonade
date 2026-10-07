#pragma once

#include <functional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/server_context.h"

namespace lemon {

using ModelOptionsMutation =
    std::function<bool(const std::string& model_key, ModelInfo& info, httplib::Response& res)>;

// The saved, effective and default options that every models/{id}/options route returns.
nlohmann::json model_options_schema();

// The three options routes resolve the model from the path, apply their mutation
// (none for GET), and answer with the saved, effective and default options. The
// mutation updates info to the state the response describes; returning false means
// it already wrote an error.
void respond_with_model_options(ServerContext& ctx, const httplib::Request& http,
                                httplib::Response& res, const ModelOptionsMutation& mutation);

} // namespace lemon
