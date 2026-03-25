#include "lemon_cli/model_selection.h"

#include "lemon/utils/aixlog.hpp"

#include <cctype>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lemon_cli {
namespace {

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

    std::cout << "Enter number: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        LOG(ERROR, "ModelSelector") << "Error: Failed to read model selection." << std::endl;
        return false;
    }

    size_t parsed_chars = 0;
    int selected = 0;
    try {
        selected = std::stoi(input, &parsed_chars);
    } catch (const std::exception&) {
        LOG(ERROR, "ModelSelector") << "Error: Invalid selection." << std::endl;
        return false;
    }

    if (parsed_chars != input.size() || selected < 1 || static_cast<size_t>(selected) > display_models.size()) {
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
                              bool show_all) {
    if (!model_out.empty()) {
        return true;
    }

    std::cout << "No model specified for '" << command_name << "'." << std::endl;
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
