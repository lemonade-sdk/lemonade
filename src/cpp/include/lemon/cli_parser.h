#pragma once

#include <CLI/CLI.hpp>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

struct ServerConfig {
    int port = 8000;
    std::string host = "localhost";
    std::string log_level = "info";
    json recipe_options = json::object();
    std::string extra_models_dir = "";  // Secondary directory for GGUF model discovery
    bool no_broadcast = false;  // Disable UDP broadcasting on private networks
    long global_timeout = 300;    // Default global timeout in seconds

    // Multi-model support: Max loaded models per type slot
    int max_loaded_models = 1;
};

class CLIParser {
public:
    CLIParser();

    // Add a flag before parsing (for caller-specific flags like --silent)
    CLI::Option* add_flag(const std::string& name, bool& value, const std::string& desc) {
        return app_.add_flag(name, value, desc);
    }

    // Parse command line arguments
    // Returns: 0 if should continue, exit code (may be 0) if should exit
    int parse(int argc, char** argv);

    // Get server configuration
    ServerConfig get_config() const { return config_; }

    // Check if we should continue (false means exit cleanly, e.g., after --help)
    bool should_continue() const { return should_continue_; }

    // Get exit code (only valid if should_continue() is false)
    int get_exit_code() const { return exit_code_; }
private:
    CLI::App app_;
    ServerConfig config_;
    bool should_continue_ = true;
    int exit_code_ = 0;
};

} // namespace lemon
