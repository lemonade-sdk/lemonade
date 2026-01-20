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
    
    // Multi-model support: Max loaded models by type
    int max_llm_models = 1;
    int max_embedding_models = 1;
    int max_reranking_models = 1;
    int max_audio_models = 1;
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

/**
    std::cout << "  --checkpoint CHECKPOINT  Hugging Face checkpoint (format: org/model:variant)\n";
    std::cout << "                           OR an absolute local path to a model directory.\n";
    std::cout << "                           When a local path is provided, files are copied to\n";
    std::cout << "                           the HuggingFace cache and registered.\n";
    std::cout << "  --recipe RECIPE          Inference recipe to use\n";
    std::cout << "                           Options: llamacpp, flm, oga-cpu, oga-hybrid, oga-npu\n";
    std::cout << "                           Required when using a local path.\n\n";
    std::cout << "  --reasoning              Mark model as a reasoning model (e.g., DeepSeek-R1)\n";
    std::cout << "                           Adds 'reasoning' label to model metadata.\n\n";
    std::cout << "  --vision                 Mark model as a vision model (multimodal)\n";
    std::cout << "                           Adds 'vision' label to model metadata.\n\n";
    std::cout << "  --embedding              Mark model as an embedding model\n";
    std::cout << "                           Adds 'embeddings' label to model metadata.\n";
    std::cout << "                           For use with /api/v1/embeddings endpoint.\n\n";
    std::cout << "  --reranking              Mark model as a reranking model\n";
    std::cout << "                           Adds 'reranking' label to model metadata.\n";
    std::cout << "                           For use with /api/v1/reranking endpoint.\n\n";
    std::cout << "  --mmproj FILENAME        Multimodal projector file for vision models\n";
    std::cout << "                           Required for GGUF vision models.\n";
    std::cout << "                           Example: mmproj-model-f16.gguf\n\n";
*/

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
    std::vector<int> max_models_vec_;  // Vector to capture --max-loaded-models values
};

} // namespace lemon
