#pragma once

#include <lemon/recipe_options.h>

#include <nlohmann/json.hpp>

#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// One model's options folded lowest-first: image defaults, the row's authored options,
// what the user saved; see SDServer::build_extra_args() for the full ladder. `authored`
// is null to read the row's own document instead. `saved_key` must be a canonical ID.
RecipeOptions build_recipe_options(const ModelInfo& info, const nlohmann::json& authored,
                                   const std::string& saved_key,
                                   const nlohmann::json& saved_options);

}  // namespace models
}  // namespace lemon
