#include <lemon/models/model_info.h>
#include <lemon/models/recipe_option_layers.h>
#include <lemon/utils/json_utils.h>

namespace lemon::models {

RecipeOptions build_recipe_options(const ModelInfo& info, const nlohmann::json& authored,
                                   const std::string& saved_key,
                                   const nlohmann::json& saved_options) {
    // Non-sd recipes pass an empty image_defaults layer.
    nlohmann::json image_defaults = nlohmann::json::object();

    if (info.image_defaults.has_defaults) {
        image_defaults["steps"] = info.image_defaults.steps;
        image_defaults["cfg_scale"] = info.image_defaults.cfg_scale;
        image_defaults["width"] = info.image_defaults.width;
        image_defaults["height"] = info.image_defaults.height;
        if (!info.image_defaults.sampling_method.empty())
            image_defaults["sampling_method"] = info.image_defaults.sampling_method;
        if (info.image_defaults.flow_shift > 0.0f)
            image_defaults["flow_shift"] = info.image_defaults.flow_shift;
    }

    nlohmann::json user_saved = utils::JsonUtils::has_key(saved_options, saved_key)
                                    ? saved_options[saved_key]
                                    : nlohmann::json(nullptr);

    return RecipeOptions::merge_precedence_layers(info.recipe, image_defaults, authored,
                                                  user_saved);
}

}  // namespace lemon::models
