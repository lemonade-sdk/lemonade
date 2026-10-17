#pragma once

#include <lemon/recipe_options.h>

#include <nlohmann/json.hpp>

#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// One model's options, folded from the precedence layers, lowest first: the document's
// image defaults, the row's authored `recipe_options`, then what the user saved. The
// full ladder, including the Router's layers above these, is documented in
// SDServer::build_extra_args().
//
// `authored` is the layer read off the row. Pass null json to have the model's own
// document read instead, which costs a lookup the caller usually already made.
//
// `saved_key` is a canonical ID (builtin.* / user.* / extra.*): saved options are keyed
// that way, so a caller holding a cache key must translate it first.
RecipeOptions build_recipe_options(const ModelInfo& info, const nlohmann::json& authored,
                                   const std::string& saved_key,
                                   const nlohmann::json& saved_options);

}  // namespace models
}  // namespace lemon
