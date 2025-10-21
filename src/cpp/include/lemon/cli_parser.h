#pragma once

#include <CLI/CLI.hpp>
#include <string>
#include <vector>

namespace lemon {

struct ServeConfig {
    int port = 8000;
    std::string host = "localhost";
    std::string log_level = "info";
    bool tray = true;
    std::string llamacpp_backend = "vulkan";
    int ctx_size = 4096;
};

struct PullConfig {
    std::vector<std::string> models;
    std::string checkpoint;
    std::string recipe;
    bool reasoning = false;
    bool vision = false;
    std::string mmproj;
};

struct DeleteConfig {
    std::vector<std::string> models;
};

struct RunConfig {
    std::string model;
    int port = 8000;
    std::string host = "localhost";
    std::string log_level = "info";
    bool tray = true;
    std::string llamacpp_backend = "vulkan";
    int ctx_size = 4096;
};

class CLIParser {
public:
    CLIParser();
    
    // Parse command line arguments
    bool parse(int argc, char** argv);
    
    // Get the command that was invoked
    std::string get_command() const { return command_; }
    
    // Get configuration based on command
    ServeConfig get_serve_config() const { return serve_config_; }
    PullConfig get_pull_config() const { return pull_config_; }
    DeleteConfig get_delete_config() const { return delete_config_; }
    RunConfig get_run_config() const { return run_config_; }
    
    // Show version
    bool should_show_version() const { return show_version_; }
    
private:
    void setup_serve_command();
    void setup_status_command();
    void setup_stop_command();
    void setup_list_command();
    void setup_pull_command();
    void setup_delete_command();
    void setup_run_command();
    
    CLI::App app_;
    std::string command_;
    
    ServeConfig serve_config_;
    PullConfig pull_config_;
    DeleteConfig delete_config_;
    RunConfig run_config_;
    
    bool show_version_ = false;
};

} // namespace lemon

