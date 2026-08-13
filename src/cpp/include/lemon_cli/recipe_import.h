#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace lemon_cli {

// True when `path` is shaped like a local .json file argument (suffix check
// only — existence is NOT verified). `pull` uses this to route the argument to
// the file path, so a typo'd filename gets a clear file-not-found error
// instead of falling through to the model/registry flow.
bool looks_like_json_file_argument(const std::string& path);

// Validate a model/policy JSON file without registering anything: the local
// structural checks import runs, plus the server-side routing-policy parse
// (POST /routing/validate) for collection.router documents. Returns 0 when
// valid. Component existence is not checked — that happens at registration.
int validate_model_json_file(lemonade::LemonadeClient& client,
                             const std::string& json_path);

// Validate and normalize imported model JSON payload.
bool validate_and_transform_model_json(nlohmann::json& model_data);

// Import a local JSON recipe/model file.
// `upgrade` mirrors pull_model's flag: explicit `pull <file>` opts into the
// registry update check like any explicit pull; `import` keeps it off.
int import_model_from_json_file(lemonade::LemonadeClient& client,
                                const std::string& json_path,
                                std::string* imported_model_out = nullptr,
                                bool upgrade = false);

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
