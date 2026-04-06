#include "lemon_cli/model_selection.h"
#include "lemon_cli/recipe_import.h"

#include "lemon/utils/aixlog.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace lemon_cli {
namespace {

bool prompt_number(const std::string& prompt, int& selected_out) {
    std::cout << prompt << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        LOG(ERROR, "ModelSelector") << "Error: Failed to read selection." << std::endl;
        return false;
    }

    size_t parsed_chars = 0;
    try {
        selected_out = std::stoi(input, &parsed_chars);
    } catch (const std::exception&) {
        LOG(ERROR, "ModelSelector") << "Error: Invalid selection." << std::endl;
        return false;
    }

    if (parsed_chars != input.size()) {
        LOG(ERROR, "ModelSelector") << "Error: Invalid selection." << std::endl;
        return false;
    }

    return true;
}

bool fetch_models_from_endpoint(lemonade::LemonadeClient& client,
                                bool show_all,
                                std::vector<lemonade::ModelInfo>& models_out) {
    try {
        std::string path = "/api/v1/models?show_all=" + std::string(show_all ? "true" : "false");
        std::string response = client.make_request(path, "GET", "", "", 3000, 3000);
        nlohmann::json json_response = nlohmann::json::parse(response);

        if (!json_response.contains("data") || !json_response["data"].is_array()) {
            return true;
        }

        for (const auto& model_item : json_response["data"]) {
            if (!model_item.is_object()) {
                continue;
            }

            lemonade::ModelInfo info;
            if (model_item.contains("id") && model_item["id"].is_string()) {
                info.id = model_item["id"].get<std::string>();
            }
            if (model_item.contains("recipe") && model_item["recipe"].is_string()) {
                info.recipe = model_item["recipe"].get<std::string>();
            }
            if (model_item.contains("downloaded") && model_item["downloaded"].is_boolean()) {
                info.downloaded = model_item["downloaded"].get<bool>();
            }
            if (model_item.contains("suggested") && model_item["suggested"].is_boolean()) {
                info.suggested = model_item["suggested"].get<bool>();
            }
            if (model_item.contains("labels") && model_item["labels"].is_array()) {
                for (const auto& label : model_item["labels"]) {
                    if (label.is_string()) {
                        info.labels.push_back(label.get<std::string>());
                    }
                }
            }

            if (!info.id.empty()) {
                models_out.push_back(info);
            }
        }

        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelSelector") << "Error: Failed to query /api/v1/models: " << e.what() << std::endl;
        return false;
    }
}

bool has_label(const lemonade::ModelInfo& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

bool is_recommended_for_launch(const lemonade::ModelInfo& model) {
    return model.recipe == "llamacpp" && has_label(model, "hot") && has_label(model, "tool-calling");
}

bool is_recommended_for_run(const lemonade::ModelInfo& model) {
    return has_label(model, "hot");
}

std::string normalize_agent_key(const std::string& agent_name) {
    std::string key;
    key.reserve(agent_name.size());
    for (char c : agent_name) {
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return key;
}

bool starts_with_case_insensitive(const std::string& value, const std::string& prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {
        const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
        const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (lhs != rhs) {
            return false;
        }
    }

    return true;
}

bool is_qwen35_family_model(const lemonade::ModelInfo& model) {
    return starts_with_case_insensitive(model.id, "Qwen3.5");
}

std::vector<std::string> preferred_recipe_directories_for_agent(const std::string& agent_name) {
    const std::string agent = normalize_agent_key(agent_name);
    if (agent == "claude" || agent == "codex") {
        return {"coding-agents"};
    }
    return {};
}

bool prompt_model_name_input(std::string& model_out) {
    std::cout << "Type model name: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        LOG(ERROR, "ModelSelector") << "Error: Failed to read model name." << std::endl;
        return false;
    }

    auto is_not_space = [](unsigned char c) {
        return !std::isspace(c);
    };

    const auto begin = std::find_if(input.begin(), input.end(), is_not_space);
    if (begin == input.end()) {
        LOG(ERROR, "ModelSelector") << "Error: Model name cannot be empty." << std::endl;
        return false;
    }

    const auto end = std::find_if(input.rbegin(), input.rend(), is_not_space).base();
    model_out = std::string(begin, end);
    std::cout << "Selected model: " << model_out << std::endl;
    return true;
}

std::vector<const lemonade::ModelInfo*> filter_recommended_launch_models(
    const std::vector<lemonade::ModelInfo>& models,
    const std::string& agent_name) {
    std::vector<const lemonade::ModelInfo*> filtered;
    filtered.reserve(models.size());
    const bool exclude_qwen35_for_codex = normalize_agent_key(agent_name) == "codex";

    for (const auto& model : models) {
        if (!is_recommended_for_launch(model)) {
            continue;
        }
        if (exclude_qwen35_for_codex && is_qwen35_family_model(model)) {
            continue;
        }

        filtered.push_back(&model);
    }
    return filtered;
}

bool prompt_launch_recipe_first(lemonade::LemonadeClient& client,
                                std::string& model_out,
                                const std::string& agent_name) {
    enum class MenuState {
        RecipeDirectories,
        RecipeFiles,
        DownloadedModels,
        RecommendedModels
    };

    MenuState state = MenuState::RecipeDirectories;
    std::string selected_recipe_dir;
    const bool is_codex_agent = normalize_agent_key(agent_name) == "codex";
    bool use_preferred_recipe_dir = false;
    std::string preferred_recipe_dir;
    bool remote_dirs_loaded = false;
    std::vector<std::string> remote_recipe_dirs;
    std::string remote_dirs_error;

    std::string agent_name_display = agent_name;
    if (!agent_name_display.empty()) {
        agent_name_display[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(agent_name_display[0])));
    }

    while (true) {
        if (state == MenuState::RecipeDirectories) {
            if (!remote_dirs_loaded) {
                if (!lemon_cli::list_remote_recipe_directories(remote_recipe_dirs, remote_dirs_error)) {
                    if (remote_dirs_error.empty()) {
                        remote_dirs_error = "Unknown error";
                    }
                }
                remote_dirs_loaded = true;
            }

            if (!remote_dirs_error.empty()) {
                std::cout << "Warning: Failed to fetch remote launch recipe directories: "
                          << remote_dirs_error << std::endl;
                std::cout << "Falling back to downloaded model browser." << std::endl;
                state = MenuState::DownloadedModels;
                continue;
            }

            std::vector<std::string> recipe_dirs;
            const std::vector<std::string> preferred_dirs =
                preferred_recipe_directories_for_agent(agent_name);
            if (!preferred_dirs.empty()) {
                std::unordered_set<std::string> remote_dir_set(remote_recipe_dirs.begin(),
                                                               remote_recipe_dirs.end());
                for (const auto& preferred_dir : preferred_dirs) {
                    if (remote_dir_set.find(preferred_dir) != remote_dir_set.end()) {
                        recipe_dirs.push_back(preferred_dir);
                    }
                }

                if (recipe_dirs.empty()) {
                    std::cout << "Warning: Preferred recipe directory for "
                              << agent_name_display << " was not found remotely."
                              << " Showing all available directories." << std::endl;
                    recipe_dirs = remote_recipe_dirs;
                    use_preferred_recipe_dir = false;
                    preferred_recipe_dir.clear();
                } else {
                    use_preferred_recipe_dir = (recipe_dirs.size() == 1);
                    preferred_recipe_dir = recipe_dirs[0];
                }
            } else {
                recipe_dirs = remote_recipe_dirs;
                use_preferred_recipe_dir = false;
                preferred_recipe_dir.clear();
            }

            if (use_preferred_recipe_dir) {
                selected_recipe_dir = preferred_recipe_dir;
                state = MenuState::RecipeFiles;
                continue;
            }

            if (!agent_name_display.empty()) {
                std::cout << "Select a recipe directory to import and use with "
                          << agent_name_display << ":" << std::endl;
            } else {
                std::cout << "Select a recipe directory to import and use:" << std::endl;
            }

            std::cout << "  0) Browse downloaded models" << std::endl;
            for (size_t i = 0; i < recipe_dirs.size(); ++i) {
                std::cout << "  " << (i + 1) << ") " << recipe_dirs[i] << std::endl;
            }

            if (recipe_dirs.empty()) {
                std::cout << "No recipe directories found. Use option 0 to browse models."
                          << std::endl;
            }

            int selected = 0;
            if (!prompt_number("Enter number: ", selected)) {
                return false;
            }

            if (selected == 0) {
                state = MenuState::DownloadedModels;
                continue;
            }
            if (selected < 1 || static_cast<size_t>(selected) > recipe_dirs.size()) {
                LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
                return false;
            }

            selected_recipe_dir = recipe_dirs[static_cast<size_t>(selected - 1)];
            state = MenuState::RecipeFiles;
            continue;
        }

        if (state == MenuState::RecipeFiles) {
            std::vector<std::string> recipe_files;
            std::string fetch_error;
            if (!lemon_cli::list_remote_recipe_files(selected_recipe_dir, recipe_files, fetch_error)) {
                std::cout << "Warning: Failed to fetch recipes in '" << selected_recipe_dir
                          << "': " << fetch_error << std::endl;
                state = MenuState::RecipeDirectories;
                continue;
            }

            const bool in_preferred_recipe_dir =
                use_preferred_recipe_dir && selected_recipe_dir == preferred_recipe_dir;

            if (!agent_name_display.empty()) {
                if (in_preferred_recipe_dir) {
                    std::cout << "Select a recipe to import and use with "
                              << agent_name_display << ":" << std::endl;
                } else {
                    std::cout << "Select a recipe from '" << selected_recipe_dir
                              << "' to import and use with " << agent_name_display << ":"
                              << std::endl;
                }
            } else {
                std::cout << "Select a recipe from '" << selected_recipe_dir
                          << "' to import and use:" << std::endl;
            }

            if (is_codex_agent) {
                std::cout
                    << "\nWarning: Qwen 3.5 family models currently do not work with Codex due to "
                    << "a llama.cpp incompatibility. Track upstream: "
                    << "https://github.com/ggml-org/llama.cpp/issues/20733\n"
                    << std::endl;
            }

            if (in_preferred_recipe_dir) {
                std::cout << "  0) Browse downloaded models" << std::endl;
            } else {
                std::cout << "  0) Back to recipe directories" << std::endl;
            }
            for (size_t i = 0; i < recipe_files.size(); ++i) {
                std::cout << "  " << (i + 1) << ") " << recipe_files[i] << std::endl;
            }

            if (recipe_files.empty()) {
                std::cout << "No recipe files found under '" << selected_recipe_dir << "'.";
                if (in_preferred_recipe_dir) {
                    std::cout << " Use option 0 to browse downloaded models.";
                } else {
                    std::cout << " Use option 0 to pick another directory.";
                }
                std::cout << std::endl;
            }

            int selected = 0;
            if (!prompt_number("Enter number: ", selected)) {
                return false;
            }

            if (selected == 0) {
                if (in_preferred_recipe_dir) {
                    state = MenuState::DownloadedModels;
                } else {
                    state = MenuState::RecipeDirectories;
                }
                continue;
            }
            if (selected < 1 || static_cast<size_t>(selected) > recipe_files.size()) {
                LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
                return false;
            }

            const std::string selected_recipe_file = recipe_files[static_cast<size_t>(selected - 1)];
            std::string imported_model;
            int import_result = lemon_cli::import_remote_recipe(
                client,
                selected_recipe_dir,
                selected_recipe_file,
                true,
                true,
                &imported_model,
                false);
            if (import_result != 0) {
                return false;
            }

            if (imported_model.empty()) {
                LOG(ERROR, "ModelSelector")
                    << "Error: Selected recipe did not return an imported model." << std::endl;
                return false;
            }

            model_out = imported_model;
            std::cout << "Using imported recipe model: " << model_out << std::endl;
            return true;
        }

        if (state == MenuState::DownloadedModels) {
            std::vector<lemonade::ModelInfo> downloaded_models;
            if (!fetch_models_from_endpoint(client, false, downloaded_models)) {
                return false;
            }
            std::vector<const lemonade::ModelInfo*> downloaded_llamacpp_models;
            downloaded_llamacpp_models.reserve(downloaded_models.size());
            for (const auto& model : downloaded_models) {
                if (model.recipe == "llamacpp") {
                    downloaded_llamacpp_models.push_back(&model);
                }
            }

            if (is_codex_agent) {
                std::cout
                    << "\nWarning: Qwen 3.5 family models currently do not work with Codex due to "
                    << "a llama.cpp incompatibility. Track upstream: "
                    << "https://github.com/ggml-org/llama.cpp/issues/20733\n"
                    << std::endl;
            }

            std::cout << "Browse downloaded llamacpp models:" << std::endl;
            std::cout << "  0) Browse recommended models (download may be required)" << std::endl;
            for (size_t i = 0; i < downloaded_llamacpp_models.size(); ++i) {
                const auto& model = *downloaded_llamacpp_models[i];
                std::cout << "  " << (i + 1) << ") " << model.id
                          << " [downloaded]"
                          << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                          << std::endl;
            }
            const int back_to_recipe_dirs = static_cast<int>(downloaded_llamacpp_models.size()) + 1;
            if (use_preferred_recipe_dir) {
                std::cout << "  " << back_to_recipe_dirs << ") Back to recipes" << std::endl;
            } else {
                std::cout << "  " << back_to_recipe_dirs << ") Back to recipe directories"
                          << std::endl;
            }

            if (downloaded_llamacpp_models.empty()) {
                std::cout << "No downloaded llamacpp models found." << std::endl;
            }

            int selected = 0;
            if (!prompt_number("Enter number: ", selected)) {
                return false;
            }

            if (selected == 0) {
                state = MenuState::RecommendedModels;
                continue;
            }
            if (selected == back_to_recipe_dirs) {
                state = MenuState::RecipeDirectories;
                continue;
            }
            if (selected < 1 || static_cast<size_t>(selected) > downloaded_llamacpp_models.size()) {
                LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
                return false;
            }

            model_out = downloaded_llamacpp_models[static_cast<size_t>(selected - 1)]->id;
            std::cout << "Selected model: " << model_out << std::endl;
            return true;
        }

        if (state == MenuState::RecommendedModels) {
            std::vector<lemonade::ModelInfo> all_models;
            if (!fetch_models_from_endpoint(client, true, all_models)) {
                return false;
            }

            std::vector<const lemonade::ModelInfo*> recommended_all =
                filter_recommended_launch_models(all_models, agent_name);

            std::cout << "Browse recommended models (llamacpp + hot + tool-calling):" << std::endl;
            std::cout << "  0) Back to downloaded models" << std::endl;
            for (size_t i = 0; i < recommended_all.size(); ++i) {
                const auto& model = *recommended_all[i];
                std::cout << "  " << (i + 1) << ") " << model.id
                          << " [" << (model.downloaded ? "downloaded" : "not-downloaded") << "]"
                          << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                          << std::endl;
            }

            if (recommended_all.empty()) {
                std::cout << "No recommended models available right now." << std::endl;
            }

            int selected = 0;
            if (!prompt_number("Enter number: ", selected)) {
                return false;
            }

            if (selected == 0) {
                state = MenuState::DownloadedModels;
                continue;
            }
            if (selected < 1 || static_cast<size_t>(selected) > recommended_all.size()) {
                LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
                return false;
            }

            model_out = recommended_all[static_cast<size_t>(selected - 1)]->id;
            std::cout << "Selected model: " << model_out << std::endl;
            return true;
        }
    }
}

bool prompt_model_selection(lemonade::LemonadeClient& client,
                            std::string& model_out,
                            bool show_all) {
    std::vector<lemonade::ModelInfo> models;
    if (!fetch_models_from_endpoint(client, false, models)) {
        return false;
    }
    if (models.empty() && !fetch_models_from_endpoint(client, true, models)) {
        return false;
    }

    if (models.empty()) {
        LOG(ERROR, "ModelSelector") << "No models available on server. Try 'lemonade list' or 'lemonade pull <MODEL>'." << std::endl;
        return false;
    }

    std::vector<const lemonade::ModelInfo*> display_models;
    display_models.reserve(models.size());
    for (const auto& model : models) {
        if (!show_all && model.recipe != "llamacpp") {
            continue;
        }
        display_models.push_back(&model);
    }

    if (display_models.empty()) {
        LOG(ERROR, "ModelSelector") << "No models available for the current filter." << std::endl;
        return false;
    }

    std::cout << "Select a model:" << std::endl;
    for (size_t i = 0; i < display_models.size(); ++i) {
        const auto& model = *display_models[i];

        std::cout << "  " << (i + 1) << ") " << model.id
                  << " [" << (model.downloaded ? "downloaded" : "not-downloaded") << "]"
                  << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                  << std::endl;
    }

    int selected = 0;
    if (!prompt_number("Enter number: ", selected)) {
        return false;
    }

    if (selected < 1 || static_cast<size_t>(selected) > display_models.size()) {
        LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
        return false;
    }

    model_out = display_models[static_cast<size_t>(selected - 1)]->id;
    std::cout << "Selected model: " << model_out << std::endl;
    return true;
}

} // namespace

bool resolve_model_if_missing(lemonade::LemonadeClient& client,
                              std::string& model_out,
                              const std::string& command_name,
                              bool show_all,
                              const std::string& agent_name) {
    if (!model_out.empty()) {
        return true;
    }

    std::cout << "No model specified for '" << command_name << "'." << std::endl;

    if (command_name == "launch") {
        return prompt_launch_recipe_first(client, model_out, agent_name);
    }

    if (command_name == "run") {
        std::vector<lemonade::ModelInfo> all_models;
        if (!fetch_models_from_endpoint(client, true, all_models)) {
            return false;
        }

        std::vector<const lemonade::ModelInfo*> hot_models;
        hot_models.reserve(all_models.size());
        for (const auto& model : all_models) {
            if (is_recommended_for_run(model)) {
                hot_models.push_back(&model);
            }
        }

        std::sort(hot_models.begin(), hot_models.end(), [](const lemonade::ModelInfo* lhs,
                                                           const lemonade::ModelInfo* rhs) {
            return lhs->id < rhs->id;
        });

        if (hot_models.empty()) {
            LOG(ERROR, "ModelSelector")
                << "No hot models available. Try 'lemonade list' or 'lemonade pull <MODEL>'."
                << std::endl;
            return false;
        }

        std::cout << "Select a hot model to run:" << std::endl;
        std::cout << "  0) Type the name of any model" << std::endl;
        for (size_t i = 0; i < hot_models.size(); ++i) {
            const auto& model = *hot_models[i];
            std::cout << "  " << (i + 1) << ") " << model.id
                      << " [" << (model.downloaded ? "downloaded" : "not-downloaded") << "]"
                      << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                      << std::endl;
        }

        int selected = 0;
        if (!prompt_number("Enter number: ", selected)) {
            return false;
        }

        if (selected == 0) {
            return prompt_model_name_input(model_out);
        }
        if (selected < 1 || static_cast<size_t>(selected) > hot_models.size()) {
            LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
            return false;
        }

        model_out = hot_models[static_cast<size_t>(selected - 1)]->id;
        std::cout << "Selected model: " << model_out << std::endl;
        return true;
    }

    return prompt_model_selection(client, model_out, show_all);
}

bool prompt_yes_no(const std::string& prompt, bool default_yes) {
    std::cout << prompt << (default_yes ? " [Y/n]: " : " [y/N]: ") << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        return default_yes;
    }

    if (input.empty()) {
        return default_yes;
    }

    for (char& c : input) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (input == "y" || input == "yes") {
        return true;
    }
    if (input == "n" || input == "no") {
        return false;
    }

    return default_yes;
}

} // namespace lemon_cli
