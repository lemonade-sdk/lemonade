#pragma once

#include <CLI/CLI.hpp>
#include <string>

namespace lemon {

struct ServerConfig {
    std::string cache_dir;     // Positional arg: lemonade cache dir (optional, platform default)
    int port = -1;             // -1 = not specified on CLI, use config.json value
    std::string host;          // Empty = not specified on CLI, use config.json value
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
