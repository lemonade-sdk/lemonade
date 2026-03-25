#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <string>

namespace lemon_cli {

// Validate and normalize imported model JSON payload.
bool validate_and_transform_model_json(nlohmann::json& model_data);

// Import a local JSON recipe/model file.
int import_model_from_json_file(lemonade::LemonadeClient& client,
                                const std::string& json_path,
                                std::string* imported_model_out = nullptr);

// Import a remote recipe from lemonade-sdk/recipes on GitHub.
int import_remote_recipe(lemonade::LemonadeClient& client,
                         const std::string& repo_dir,
                         const std::string& recipe_file,
                         bool skip_prompt,
                         bool yes,
                         std::string* imported_model_out,
                         bool allow_skip);

} // namespace lemon_cli
