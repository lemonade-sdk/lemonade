#pragma once

#include <string>

#include <nlohmann/json.hpp>

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

} // namespace lemon
