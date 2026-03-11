#include "lemon_cli/lemonade_client.h"
#include <lemon/recipe_options.h>
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>

static const std::vector<std::string> VALID_LABELS = { "coding", "embeddings", "hot", "reasoning", "reranking", "tool-calling", "vision"};

static bool validate_and_transform_model_json(nlohmann::json& model_data) {
    // Validate model_name (or id -> model_name)
    if (!model_data.contains("model_name") || !model_data["model_name"].is_string()) {
        if (model_data.contains("id") && model_data["id"].is_string()) {
            model_data["model_name"] = model_data["id"];
            model_data.erase("id");
        } else {
            std::cerr << "Error: JSON file must contain a 'model_name' string field" << std::endl;
            return false;
        }
    }

    // Validate recipe
    if (!model_data.contains("recipe") || !model_data["recipe"].is_string()) {
        std::cerr << "Error: JSON file must contain a 'recipe' string field" << std::endl;
        return false;
    }

    // Validate checkpoints or checkpoint
    bool has_checkpoints = model_data.contains("checkpoints") && model_data["checkpoints"].is_array();
    bool has_checkpoint = model_data.contains("checkpoint") && model_data["checkpoint"].is_string();
    if (!has_checkpoints && !has_checkpoint) {
        std::cerr << "Error: JSON file must contain either 'checkpoints' (array) or 'checkpoint' (string)" << std::endl;
        return false;
    }

    // If both checkpoints and checkpoint exist, remove checkpoint
    if (has_checkpoints && has_checkpoint) {
        model_data.erase("checkpoint");
    }

    return true;
}

static bool handle_backend_operation(const std::string& spec, const std::string& operation_name,
                                    std::function<int(const std::string&, const std::string&)> action) {
    if (spec.empty()) {
        return false;
    }
    size_t colon_pos = spec.find(':');
    if (colon_pos == std::string::npos) {
        std::cerr << "Error: " << operation_name << " requires recipe:backend format (e.g., llamacpp:vulkan)" << std::endl;
        return true;
    }
    std::string recipe_name = spec.substr(0, colon_pos);
    std::string backend_name = spec.substr(colon_pos + 1);
    action(recipe_name, backend_name);
    return true;
}

static int handle_pull_command(lemonade::LemonadeClient& client, const lemonade::CliConfig& config) {
    nlohmann::json model_data;
    bool use_json_file = false;

    // Check if model is a path to a JSON file
    if (config.model.length() > 5 && config.model.substr(config.model.length() - 5) == ".json") {
        // Try to load JSON from file
        std::ifstream file(config.model);
        if (file.good()) {
            try {
                model_data = nlohmann::json::parse(file);
                file.close();
                use_json_file = true;

                if (!validate_and_transform_model_json(model_data)) {
                    return 1;
                }
            } catch (const nlohmann::json::exception& e) {
                std::cerr << "Error: Failed to parse JSON file '" << config.model << "': " << e.what() << std::endl;
                return 1;
            }
        } else {
            // File doesn't exist, fall back to treating as model name
            file.close();
        }
    }

    if (!use_json_file) {
        // Build model_data JSON from command line options
        model_data["model_name"] = config.model;
        model_data["recipe"] = config.recipe;

        if (!config.checkpoints.empty()) {
            model_data["checkpoints"] = config.checkpoints;
        }

        if (!config.labels.empty()) {
            model_data["labels"] = config.labels;
        }
    }

    return client.pull_model(model_data);
}

static int handle_recipes_command(lemonade::LemonadeClient& client, const lemonade::CliConfig& config) {
    if (handle_backend_operation(config.install_backend, "Install",
        [&client](const std::string& recipe, const std::string& backend) {
            return client.install_backend(recipe, backend);
        })) {
            return 0;
    } else if (handle_backend_operation(config.uninstall_backend, "Uninstall",
        [&client](const std::string& recipe, const std::string& backend) {
            return client.uninstall_backend(recipe, backend);
        })) {
            return 0;
    }

    return client.list_recipes();
}

int main(int argc, char* argv[]) {
    // CLI11 configuration
    CLI::App app{"Lemonade CLI - HTTP client for Lemonade Server"};

    // Create config object and bind CLI11 options directly to it
    lemonade::CliConfig config;

    // Set up CLI11 options with callbacks that write directly to config
    app.set_help_flag("--help,-h", "Display help information");
    app.set_help_all_flag("--help-all", "Display help information for all subcommands");

    // Global options (available to all subcommands)
    app.add_option("--host", config.host, "Server host")->default_val(config.host);
    app.add_option("--port", config.port, "Server port")->default_val(config.port);
    app.add_option("--api-key", config.api_key, "API key for authentication")->default_val(config.api_key)->envname("LEMONADE_API_KEY");

    // Subcommands
    CLI::App* status_cmd = app.add_subcommand("status", "Check server status");
    CLI::App* list_cmd = app.add_subcommand("list", "List available models");
    CLI::App* pull_cmd = app.add_subcommand("pull", "Pull/download a model");
    CLI::App* delete_cmd = app.add_subcommand("delete", "Delete a model");
    CLI::App* load_cmd = app.add_subcommand("load", "Load a model into memory");
    CLI::App* unload_cmd = app.add_subcommand("unload", "Unload a model (or all models)");
    CLI::App* recipes_cmd = app.add_subcommand("recipes", "List available recipes and backends");

    // Positional model argument for pull, delete, load, unload
    pull_cmd->add_option("model", config.model, "Model name to pull")->required();
    delete_cmd->add_option("model", config.model, "Model name to delete")->required();
    load_cmd->add_option("model", config.model, "Model name to load")->required();
    unload_cmd->add_option("model", config.model, "Model name to unload");

    // Install/uninstall options for recipes command
    recipes_cmd->add_option("--install", config.install_backend, "Install a backend (recipe:backend)");
    recipes_cmd->add_option("--uninstall", config.uninstall_backend, "Uninstall a backend (recipe:backend)");

    // Pull-specific options
    pull_cmd->add_option("--checkpoint", config.checkpoints, "Model checkpoint path")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    pull_cmd->add_option("--recipe", config.recipe, "Model recipe (e.g., llamacpp)")->default_val(config.recipe);
    pull_cmd->add_option("--label", config.labels, "Filter models by labels (e.g., reasoning, coding, vision)")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->check(CLI::IsMember(VALID_LABELS));

    // Load-specific options
    lemon::RecipeOptions::add_cli_options(*load_cmd, config.recipe_options);
    load_cmd->add_flag("--save-options", config.save_options, "Save model options for future loads")->default_val(config.save_options);

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

    // Create client
    lemonade::LemonadeClient client(config.host, config.port, config.api_key);

    // Execute command
    if (status_cmd->count() > 0) {
        return client.status();
    } else if (list_cmd->count() > 0) {
        return client.list_models(true);
    } else if (pull_cmd->count() > 0) {
        return handle_pull_command(client, config);
    } else if (delete_cmd->count() > 0) {
        return client.delete_model(config.model);
    } else if (load_cmd->count() > 0) {
        return client.load_model(config.model, config.recipe_options, config.save_options);
    } else if (unload_cmd->count() > 0) {
        return client.unload_model(config.model);
    } else if (recipes_cmd->count() > 0) {
        return handle_recipes_command(client, config);
    } else {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << app.help() << std::endl;
        return 1;
    }
}
