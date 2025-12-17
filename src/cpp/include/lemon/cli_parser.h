#pragma once

#include <CLI/CLI.hpp>
#include <string>
#include <vector>

namespace lemon {

struct ServerConfig {
    int port = 8000;
    std::string host = "localhost";
    std::string log_level = "info";
    bool tray = false;  // Tray is handled by lemonade-server, not lemonade-router
    std::string llamacpp_backend = "vulkan";
    int ctx_size = 4096;
    std::string llamacpp_args = "";
    std::string extra_models_dir = "";  // Secondary directory for GGUF model discovery
    
    // Multi-model support: Max loaded models by type
    int max_llm_models = 1;
    int max_embedding_models = 1;
    int max_reranking_models = 1;
    int max_audio_models = 1;
};

class CLIParser {
public:
    CLIParser();
    
    // Parse command line arguments
    // Returns: 0 if should continue, exit code (may be 0) if should exit
    int parse(int argc, char** argv);
    
    // Get server configuration
    ServerConfig get_config() const { return config_; }
    
    // Check if we should continue (false means exit cleanly, e.g., after --help)
    bool should_continue() const { return should_continue_; }
    
    // Get exit code (only valid if should_continue() is false)
    int get_exit_code() const { return exit_code_; }
    
    // Show version
    bool should_show_version() const { return show_version_; }
    
private:
    CLI::App app_;
    ServerConfig config_;
    bool show_version_ = false;
    bool should_continue_ = true;
    int exit_code_ = 0;
    std::vector<int> max_models_vec_;  // Vector to capture --max-loaded-models values
};

} // namespace lemon
