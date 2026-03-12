#include "lemon_cli/lemonade_client.h"
#include <lemon/recipe_options.h>
#include <lemon/version.h>
#include <lemon_tray/agent_launcher.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>

static const std::vector<std::string> VALID_LABELS = {
    "coding",
    "embeddings",
    "hot",
    "reasoning",
    "reranking",
    "tool-calling",
    "vision"
};

static const std::vector<std::string> KNOWN_KEYS = {
    "checkpoint",
    "checkpoints",
    "model_name",
    "image_defaults",
    "labels",
    "recipe",
    "recipe_options",
    "size"
};

static const std::vector<std::string> SUPPORTED_AGENTS = {
    "claude",
    "codex"
};

// Configuration structure for CLI options
struct CliConfig {
    std::string host = "127.0.0.1";
    int port = 8000;
    std::string api_key;
    std::string model;
    std::map<std::string, std::string> checkpoints;
    std::string recipe;
    std::vector<std::string> labels;
    nlohmann::json recipe_options;
    bool save_options = false;
    std::string install_backend;  // Format: "recipe:backend"
    std::string uninstall_backend;  // Format: "recipe:backend"
    std::string output_file;
    bool downloaded = false;
    std::string agent;
};

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

    // Prepend "user." to model_name if it doesn't already start with "user."
    std::string model_name = model_data["model_name"].get<std::string>();
    if (model_name.substr(0, 5) != "user.") {
        model_data["model_name"] = "user." + model_name;
    }

    // Validate recipe
    if (!model_data.contains("recipe") || !model_data["recipe"].is_string()) {
        std::cerr << "Error: JSON file must contain a 'recipe' string field" << std::endl;
        return false;
    }

    // Validate checkpoints or checkpoint
    bool has_checkpoints = model_data.contains("checkpoints") && model_data["checkpoints"].is_object();
    bool has_checkpoint = model_data.contains("checkpoint") && model_data["checkpoint"].is_string();
    if (!has_checkpoints && !has_checkpoint) {
        std::cerr << "Error: JSON file must contain either 'checkpoints' (object) or 'checkpoint' (string)" << std::endl;
        return false;
    }

    // If both checkpoints and checkpoint exist, remove checkpoint
    if (has_checkpoints && has_checkpoint) {
        model_data.erase("checkpoint");
    }

    // Remove unrecognized top-level keys after validation
    std::vector<std::string> keys_to_remove;
    for (auto& [key, _] : model_data.items()) {
        bool is_known = false;
        for (const auto& known_key : KNOWN_KEYS) {
            if (key == known_key) {
                is_known = true;
                break;
            }
        }
        if (!is_known) {
            keys_to_remove.push_back(key);
        }
    }

    for (const auto& key : keys_to_remove) {
        model_data.erase(key);
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

static int handle_import_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_data;

    // Load JSON from file
    std::ifstream file(config.model);
    if (!file.good()) {
        std::cerr << "Error: Failed to open JSON file '" << config.model << "'" << std::endl;
        return 1;
    }

    try {
        model_data = nlohmann::json::parse(file);
        file.close();

        if (!validate_and_transform_model_json(model_data)) {
            return 1;
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Error: Failed to parse JSON file '" << config.model << "': " << e.what() << std::endl;
        return 1;
    }

    return client.pull_model(model_data);
}

static int handle_pull_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_data;

    // Build model_data JSON from command line options
    model_data["model_name"] = config.model;
    model_data["recipe"] = config.recipe;

    if (!config.checkpoints.empty()) {
        model_data["checkpoints"] = config.checkpoints;
    }

    if (!config.labels.empty()) {
        model_data["labels"] = config.labels;
    }

    return client.pull_model(model_data);
}

static int handle_export_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_json = client.get_model_info(config.model);

    if (model_json.empty()) {
        std::cerr << "Error: Failed to fetch model info for '" << config.model << "'" << std::endl;
        return 1;
    }

    if (!validate_and_transform_model_json(model_json)) {
        return 1;
    }

    std::string output = model_json.dump(4);

    if (config.output_file.empty()) {
        std::cout << output << std::endl;
    } else {
        std::ofstream file(config.output_file);
        if (!file.is_open()) {
            std::cerr << "Error: Failed to open output file '" << config.output_file << "'" << std::endl;
            return 1;
        }
        file << output;
        file.close();
        std::cout << "Model info exported to '" << config.output_file << "'" << std::endl;
    }

    return 0;
}

static int handle_load_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    // First, check if the model is downloaded
    nlohmann::json model_info = client.get_model_info(config.model);

    if (model_info.empty()) {
        std::cerr << "Error: Failed to fetch model info for '" << config.model << "'" << std::endl;
        return 1;
    }

    // Check if model is downloaded
    if (!model_info.contains("downloaded") || !model_info["downloaded"].is_boolean()) {
        std::cerr << "Error: Failed to determine download status for model '" << config.model << "'" << std::endl;
        return 1;
    }

    bool is_downloaded = model_info["downloaded"].get<bool>();

    if (!is_downloaded) {
        std::cout << "Model '" << config.model << "' is not downloaded. Pulling..." << std::endl;
        nlohmann::json pull_request;
        pull_request["model_name"] = config.model;
        int pull_result = client.pull_model(pull_request);
        if (pull_result != 0) {
            std::cerr << "Error: Failed to pull model '" << config.model << "'" << std::endl;
            return pull_result;
        }
        std::cout << "Model pulled successfully." << std::endl;
    }

    // Proceed with loading the model
    return client.load_model(config.model, config.recipe_options, config.save_options);
}

static int handle_recipes_command(lemonade::LemonadeClient& client, const CliConfig& config) {
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

static int handle_launch_command(const CliConfig& config) {
    lemon_tray::AgentConfig agent_config;
    std::string config_error;

    // Build agent config
    if (!lemon_tray::build_agent_config(config.agent, config.host, config.port, config.model,
                                         agent_config, config_error)) {
        std::cerr << "Failed to build agent config: " << config_error << std::endl;
        return 1;
    }

    // Find agent binary
    const std::string agent_binary = lemon_tray::find_agent_binary(agent_config);
    if (agent_binary.empty()) {
        std::cerr << "Agent binary not found for " << config.agent << std::endl;
        if (!agent_config.install_instructions.empty()) {
            std::cerr << agent_config.install_instructions << std::endl;
        }
        return 1;
    }

    // Preload model (and check if server reachable)
    lemonade::LemonadeClient client(config.host, config.port, config.api_key);
    if (client.load_model(config.model, config.recipe_options)) {
        return 1;
    }

    std::cout << "Launching " << config.agent << "..." << std::endl;

    // Launch agent process
    lemon::utils::ProcessHandle handle;
    try {
        handle = lemon::utils::ProcessManager::start_process(
            agent_binary,
            agent_config.extra_args,
            "",
            true,
            false,
            agent_config.env_vars);
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to launch agent process: " << e.what() << std::endl;
        return 1;
    }

    return lemon::utils::ProcessManager::wait_for_exit(handle, -1);
}

int main(int argc, char* argv[]) {
    // CLI11 configuration
    CLI::App app{"Lemonade CLI - HTTP client for Lemonade Server"};

    // Create config object and bind CLI11 options directly to it
    CliConfig config;

    // Set up CLI11 options with callbacks that write directly to config
    app.set_help_flag("--help,-h", "Display help information");
    app.set_help_all_flag("--help-all", "Display help information for all subcommands");
    app.set_version_flag("--version,-v", ("lemonade version " LEMON_VERSION_STRING));
    app.fallthrough(true);

    // Global options (available to all subcommands)
    app.add_option("--host", config.host, "Server host")->default_val(config.host)->type_name("HOST")->envname("LEMONADE_HOST");
    app.add_option("--port", config.port, "Server port")->default_val(config.port)->type_name("PORT")->envname("LEMONADE_PORT");
    app.add_option("--api-key", config.api_key, "API key for authentication")
        ->default_val(config.api_key)
        ->type_name("KEY")
        ->envname("LEMONADE_API_KEY");

    // Subcommands
    CLI::App* status_cmd = app.add_subcommand("status", "Check server status");
    CLI::App* list_cmd = app.add_subcommand("list", "List available models");
    CLI::App* pull_cmd = app.add_subcommand("pull", "Pull/download a model");
    CLI::App* import_cmd = app.add_subcommand("import", "Import a model from JSON file");
    CLI::App* delete_cmd = app.add_subcommand("delete", "Delete a model");
    CLI::App* load_cmd = app.add_subcommand("load", "Load a model");
    CLI::App* unload_cmd = app.add_subcommand("unload", "Unload a model (or all models)");
    CLI::App* recipes_cmd = app.add_subcommand("recipes", "List available recipes and backends");
    CLI::App* export_cmd = app.add_subcommand("export", "Export model information to JSON");
    CLI::App* launch_cmd = app.add_subcommand("launch", "Launch an agent with a model");

    // List options
    list_cmd->add_flag("--downloaded", config.downloaded, "Save model options for future loads");

    // Install/uninstall options for recipes command
    recipes_cmd->add_option("--install", config.install_backend, "Install a backend (recipe:backend)")->type_name("SPEC");
    recipes_cmd->add_option("--uninstall", config.uninstall_backend, "Uninstall a backend (recipe:backend)")->type_name("SPEC");

    // Pull options
    pull_cmd->add_option("model", config.model, "Model name to pull")->required()->type_name("MODEL");
    pull_cmd->add_option("--checkpoint", config.checkpoints, "Model checkpoint path")
        ->type_name("TYPE CHECKPOINT")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    pull_cmd->add_option("--recipe", config.recipe, "Model recipe (e.g., llamacpp, flm, sd-cpp, whispercpp)")
        ->type_name("RECIPE")
        ->default_val(config.recipe);
    pull_cmd->add_option("--label", config.labels, "Add label to model")
        ->type_name("LABEL")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->check(CLI::IsMember(VALID_LABELS));

    // Import options
    import_cmd->add_option("json_file", config.model, "Path to JSON file")->required()->type_name("JSON_FILE");

    // Delete options
    delete_cmd->add_option("model", config.model, "Model name to delete")->required()->type_name("MODEL");

    // Load options
    load_cmd->add_option("model", config.model, "Model name to load")->required()->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*load_cmd, config.recipe_options);
    load_cmd->add_flag("--save-options", config.save_options, "Save model options for future loads");

    // Unload options
    unload_cmd->add_option("model", config.model, "Model name to unload")->type_name("MODEL");

    // Export options
    export_cmd->add_option("model", config.model, "Model name to export")->type_name("MODEL")->required();
    export_cmd->add_option("--output", config.output_file, "Output file path (prints to stdout if not specified)")->type_name("PATH");

    // Launch command: agent is positional, model is --model flag
    launch_cmd->add_option("agent", config.agent, "Agent name to launch")
        ->required()
        ->type_name("AGENT")
        ->check(CLI::IsMember(SUPPORTED_AGENTS));
    launch_cmd->add_option("--model", config.model, "Model name to load")->required()->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*launch_cmd, config.recipe_options);

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

    // Create client
    lemonade::LemonadeClient client(config.host, config.port, config.api_key);

    // Execute command
    if (status_cmd->count() > 0) {
        return client.status();
    } else if (list_cmd->count() > 0) {
        return client.list_models(!config.downloaded);
    } else if (pull_cmd->count() > 0) {
        return handle_pull_command(client, config);
    } else if (import_cmd->count() > 0) {
        return handle_import_command(client, config);
    } else if (delete_cmd->count() > 0) {
        return client.delete_model(config.model);
    } else if (load_cmd->count() > 0) {
        return handle_load_command(client, config);
    } else if (unload_cmd->count() > 0) {
        return client.unload_model(config.model);
    } else if (export_cmd->count() > 0) {
        return handle_export_command(client, config);
    } else if (recipes_cmd->count() > 0) {
        return handle_recipes_command(client, config);
    } else if (launch_cmd->count() > 0) {
        return handle_launch_command(config);
    } else {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << app.help() << std::endl;
        return 1;
    }
}
