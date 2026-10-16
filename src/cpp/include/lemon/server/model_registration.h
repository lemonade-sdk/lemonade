#pragma once

#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_error.h"
#include "lemon/server/server_context.h"

namespace lemon {

// /v1/models/register and /v1/pull enter the same registration path, so a definition
// both accept is validated and stored the same way. Throws std::invalid_argument for a
// definition the caller should fix. Returns the model's public name.
//   require_definition:    /models/register requires a recipe and replaces an existing entry.
//   allow_embedded_models: /pull accepts a collection file's embedded "models" array.
//   local_import:          /pull registers files already copied into the Hugging Face cache.
std::string register_model_definition(ServerContext& ctx, const std::string& model_name,
                                      nlohmann::json& request, bool require_definition,
                                      bool allow_embedded_models, bool local_import);

extern const RouteError kInvalidModelDefinition;

// Calls register_model_definition, answering a std::invalid_argument with
// kInvalidModelDefinition. Returns the public name, or std::nullopt after writing the error.
std::optional<std::string> try_register_model_definition(
    ServerContext& ctx, const std::string& model_name, nlohmann::json& request,
    bool require_definition, bool allow_embedded_models, bool local_import,
    httplib::Response& res);

extern const RouteError kDeleteFailed;

// /v1/delete and Ollama's /api/delete remove a model the same way: unload it so the backend
// releases its file locks, then delete its files, retrying while a download cancelled just
// before still holds them. Throws what ModelManager::delete_model throws.
void delete_model_files(ServerContext& ctx, const std::string& model_name);

// Answers a delete_model_files failure: not_found for an unknown or unsupported model,
// otherwise kDeleteFailed. The body is {"error": what}.
void write_delete_error(const std::exception& error, const RouteError& not_found,
                        httplib::Response& res);

} // namespace lemon
