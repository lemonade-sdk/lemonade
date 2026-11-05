#include <lemon/models/model_info.h>
#include <lemon/models/recipe_option_layers.h>
#include <lemon/utils/json_utils.h>

namespace lemon::models {

RecipeOptions build_recipe_options(const ModelInfo& info, const nlohmann::json& authored,
                                   const std::string& saved_key,
                                   const nlohmann::json& saved_options) {
    nlohmann::json user_saved = utils::JsonUtils::has_key(saved_options, saved_key)
                                    ? saved_options[saved_key]
                                    : nlohmann::json(nullptr);

    return RecipeOptions::merge_precedence_layers(info.recipe, {authored, user_saved});
}

}  // namespace lemon::models
