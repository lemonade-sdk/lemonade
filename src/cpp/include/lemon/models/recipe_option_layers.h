#pragma once

#include <lemon/recipe_options.h>

#include <nlohmann/json.hpp>

#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// One model's options folded lowest-first: what the row authored, then what the
// user saved. `saved_key` must be a canonical ID.
RecipeOptions build_recipe_options(const ModelInfo& info, const nlohmann::json& authored,
                                   const std::string& saved_key,
                                   const nlohmann::json& saved_options);

}  // namespace models
}  // namespace lemon
