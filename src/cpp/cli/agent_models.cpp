#include "lemon_cli/agent_models.h"

#include <algorithm>

namespace lemon_cli {

AgentModelEntry agent_model_entry_from_json(const std::string& id,
                                            const nlohmann::json& model,
                                            int default_context_window) {
    std::vector<std::string> labels;
    const auto model_labels = model.find("labels");
    if (model_labels != model.end() && model_labels->is_array()) {
        for (const auto& label : *model_labels) {
            if (label.is_string()) {
                labels.push_back(label.get<std::string>());
            }
        }
    }

    int context_window = default_context_window;
    const auto options = model.find("recipe_options");
    if (options != model.end() && options->is_object()) {
        // A saved ctx_size of -1 means auto, which the CLI cannot resolve.
        const auto ctx_size = options->find("ctx_size");
        if (ctx_size != options->end() && ctx_size->is_number_integer() &&
            ctx_size->get<int>() > 0) {
            context_window = ctx_size->get<int>();
        }
    }

    return {id, id + " (local)", context_window, std::move(labels)};
}

std::vector<AgentModelEntry> fetch_llm_models_for_sync(const LemondCall& call,
                                                       int context_window,
                                                       std::string* error_out) {
    std::vector<AgentModelEntry> models;

    try {
        const nlohmann::json parsed =
            nlohmann::json::parse(call("GET", "/api/v1/models?show_all=false", "", 3000));

        if (!parsed.contains("data") || !parsed["data"].is_array()) {
            return models;
        }

        for (const auto& model : parsed["data"]) {
            if (!model.is_object() || !model.contains("id") || !model["id"].is_string()) {
                continue;
            }

            AgentModelEntry entry =
                agent_model_entry_from_json(model["id"].get<std::string>(), model, context_window);
            if (std::find(entry.labels.begin(), entry.labels.end(), "chat") == entry.labels.end()) {
                continue;
            }
            models.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        if (error_out != nullptr) {
            *error_out = e.what();
        }
    }

    return models;
}

bool load_for_context_window(const LemondCall& call,
                             const std::string& model,
                             const nlohmann::json& recipe_options,
                             const PrepReport& report,
                             int& context_window) {
    context_window = 0;
    report(PrepLevel::Status, "Loading " + model + " to read its context size...");
    try {
        nlohmann::json request_body =
            recipe_options.is_object() ? recipe_options : nlohmann::json::object();
        request_body["model_name"] = model;
        request_body["save_options"] = false;
        constexpr int load_timeout_ms = 86400000;
        call("POST", "/api/v1/load", request_body.dump(), load_timeout_ms);
    } catch (const std::exception& e) {
        report(PrepLevel::Error, "Failed to load " + model + ": " + e.what());
        return false;
    }

    try {
        const nlohmann::json info =
            nlohmann::json::parse(call("GET", "/api/v1/models/" + model, "", 3000));
        const auto context_length = info.find("context_length");
        if (context_length != info.end() && context_length->is_number_integer() &&
            context_length->get<int>() > 0) {
            context_window = context_length->get<int>();
            return true;
        }
        report(PrepLevel::Warning, "The server reported no context size for " + model + ".");
    } catch (const std::exception& e) {
        report(PrepLevel::Warning,
               "Failed to read the context size of " + model + ": " + e.what());
    }
    return true;
}

} // namespace lemon_cli
