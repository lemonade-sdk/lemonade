#include "lemon_cli/model_selection.h"

#include "lemon/utils/aixlog.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
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
    return (has_label(model, "hot") && has_label(model, "tool-calling")) || model.suggested;
}

bool is_recommended_for_run(const lemonade::ModelInfo& model) {
    return has_label(model, "hot") || model.suggested;
}

bool prompt_recommended_catalog(lemonade::LemonadeClient& client,
                                std::string& model_out,
                                bool for_launch) {
    std::vector<lemonade::ModelInfo> all_models;
    if (!fetch_models_from_endpoint(client, true, all_models)) {
        return false;
    }

    std::vector<const lemonade::ModelInfo*> recommended_models;
    recommended_models.reserve(all_models.size());
    for (const auto& model : all_models) {
        if ((for_launch && is_recommended_for_launch(model)) ||
            (!for_launch && is_recommended_for_run(model))) {
            recommended_models.push_back(&model);
        }
    }

    if (recommended_models.empty()) {
        LOG(ERROR, "ModelSelector")
            << "No recommended models available. Try 'lemonade list' or 'lemonade pull <MODEL>'."
            << std::endl;
        return false;
    }

    if (for_launch) {
        std::cout << "Browse recommended models (hot + tool-calling)."
                  << " Models marked not-downloaded will be pulled automatically on load:" << std::endl;
    } else {
        std::cout << "Browse recommended hot models."
                  << " Models marked not-downloaded will be pulled automatically on load:" << std::endl;
    }

    for (size_t i = 0; i < recommended_models.size(); ++i) {
        const auto& model = *recommended_models[i];
        std::cout << "  " << (i + 1) << ") " << model.id
                  << " [" << (model.downloaded ? "downloaded" : "not-downloaded") << "]"
                  << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                  << std::endl;
    }

    int selected = 0;
    if (!prompt_number("Enter number: ", selected)) {
        return false;
    }

    if (selected < 1 || static_cast<size_t>(selected) > recommended_models.size()) {
        LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
        return false;
    }

    model_out = recommended_models[static_cast<size_t>(selected - 1)]->id;
    std::cout << "Selected model: " << model_out << std::endl;
    return true;
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

    if (command_name == "launch" || command_name == "run") {
        const bool for_launch = (command_name == "launch");
        std::vector<lemonade::ModelInfo> downloaded_models;
        if (!fetch_models_from_endpoint(client, false, downloaded_models)) {
            return false;
        }

        std::vector<const lemonade::ModelInfo*> suggested_downloaded_models;
        suggested_downloaded_models.reserve(downloaded_models.size());
        for (const auto& model : downloaded_models) {
            if ((for_launch && is_recommended_for_launch(model)) ||
                (!for_launch && is_recommended_for_run(model))) {
                suggested_downloaded_models.push_back(&model);
            }
        }

        if (for_launch) {
            std::string agent_name_display = agent_name;
            if (!agent_name_display.empty()) {
                agent_name_display[0] = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(agent_name_display[0])));
            }
            if (!agent_name_display.empty()) {
                std::cout << "Select a suggested model + recipe to use with "
                          << agent_name_display << ":" << std::endl;
            } else {
                std::cout << "Select a suggested model + recipe to use:" << std::endl;
            }
        } else {
            std::cout << "Select a suggested hot model to run:" << std::endl;
        }

        std::cout << "  0) Browse recommended models (download may be required)" << std::endl;

        for (size_t i = 0; i < suggested_downloaded_models.size(); ++i) {
            const auto& model = *suggested_downloaded_models[i];
            std::cout << "  " << (i + 1) << ") " << model.id
                      << " [downloaded]"
                      << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                      << std::endl;
        }

        const int choose_any = static_cast<int>(suggested_downloaded_models.size()) + 1;
        std::cout << "  " << choose_any << ") Choose any model" << std::endl;

        if (suggested_downloaded_models.empty()) {
            std::cout << "No downloaded suggested models found yet." << std::endl;
        }

        int selected = 0;
        if (!prompt_number("Enter number: ", selected)) {
            return false;
        }

        if (selected == 0) {
            return prompt_recommended_catalog(client, model_out, for_launch);
        }
        if (selected == choose_any) {
            return prompt_model_selection(client, model_out, show_all);
        }
        if (selected < 1 || static_cast<size_t>(selected) > suggested_downloaded_models.size()) {
            LOG(ERROR, "ModelSelector") << "Error: Selection out of range." << std::endl;
            return false;
        }

        model_out = suggested_downloaded_models[static_cast<size_t>(selected - 1)]->id;
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
