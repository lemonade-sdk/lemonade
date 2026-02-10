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

    // Multi-model support: Max loaded models per type slot
    int max_loaded_models = 1;
};

struct TrayConfig {
    std::string command;  // No default - must be explicitly specified
    // Default to headless mode on Linux (no tray support), tray mode on other platforms
#if defined(__linux__) && !defined(__ANDROID__)
    bool no_tray = true;
#else
    bool no_tray = false;
#endif

    std::string model;

    // Run options
    bool save_options = false;

    // Pull options
    std::string checkpoint = "";
    std::string recipe = "";
    std::string mmproj = "";
    bool is_reasoning = false;
    bool is_vision = false;
    bool is_embedding = false;
    bool is_reranking = false;
};

class CLIParser {
public:
    CLIParser();

    // Parse command line arguments
    // Returns: 0 if should continue, exit code (may be 0) if should exit
    int parse(int argc, char** argv);

    // Get server configuration
    ServerConfig get_config() const { return config_; }
#ifdef LEMONADE_TRAY
    // Get tray configuration
    TrayConfig get_tray_config() const { return tray_config_; }
#endif
    // Check if we should continue (false means exit cleanly, e.g., after --help)
    bool should_continue() const { return should_continue_; }

    // Get exit code (only valid if should_continue() is false)
    int get_exit_code() const { return exit_code_; }
private:
    CLI::App app_;
    ServerConfig config_;
#ifdef LEMONADE_TRAY
    TrayConfig tray_config_;
#endif
    bool should_continue_ = true;
    int exit_code_ = 0;
};

} // namespace lemon
