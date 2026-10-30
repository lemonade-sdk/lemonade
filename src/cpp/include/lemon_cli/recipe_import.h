#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace lemon_cli {

// Validate and normalize imported model JSON payload.
bool validate_and_transform_model_json(nlohmann::json& model_data);

int import_model_from_json(lemonade::LemonadeClient& client,
                           nlohmann::json model_data,
                           std::string* imported_model_out = nullptr);
bool fetch_remote_recipe_json(const std::string& url,
                              nlohmann::json& model_data,
                              std::string& error_out);

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

// List recipe directories under lemonade-sdk/recipes.
bool list_remote_recipe_directories(std::vector<std::string>& recipe_dirs_out,
                                    std::string& error_out);

// List JSON recipe files under lemonade-sdk/recipes/<repo_dir>.
bool list_remote_recipe_files(const std::string& repo_dir,
                              std::vector<std::string>& recipe_files_out,
                              std::string& error_out);

} // namespace lemon_cli
