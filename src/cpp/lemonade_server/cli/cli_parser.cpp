// CLI argument parsing for lemonade-server
// These are TrayApp methods extracted to a separate file for organization.

#include "lemon_server/tray_app.h"
#include <iostream>
#include <cstdlib>
#include <cctype>
#include <vector>

namespace lemon_server {

// ============================================================
// Environment Variable Loading
// ============================================================

void TrayApp::load_env_defaults() {
    // Helper to get environment variable with fallback
    auto getenv_or_default = [](const char* name, const std::string& default_val) -> std::string {
        const char* val = std::getenv(name);
        return val ? std::string(val) : default_val;
    };
    
    // Helper to get integer environment variable with fallback
    auto getenv_int_or_default = [](const char* name, int default_val) -> int {
        const char* val = std::getenv(name);
        if (val) {
            try {
                return std::stoi(val);
            } catch (...) {
                // Invalid integer, use default
                return default_val;
            }
        }
        return default_val;
    };
    
    // Load environment variables into config (can be overridden by command-line args)
    config_.port = getenv_int_or_default("LEMONADE_PORT", config_.port);
    config_.host = getenv_or_default("LEMONADE_HOST", config_.host);
    config_.log_level = getenv_or_default("LEMONADE_LOG_LEVEL", config_.log_level);
    config_.llamacpp_backend = getenv_or_default("LEMONADE_LLAMACPP", config_.llamacpp_backend);
    config_.ctx_size = getenv_int_or_default("LEMONADE_CTX_SIZE", config_.ctx_size);
    config_.llamacpp_args = getenv_or_default("LEMONADE_LLAMACPP_ARGS", config_.llamacpp_args);
}

// ============================================================
// Command Line Argument Parsing
// ============================================================

void TrayApp::parse_arguments(int argc, char* argv[]) {
    // Check if there's a command (non-flag argument)
    if (argc > 1 && argv[1][0] != '-') {
        config_.command = argv[1];
        
        // Parse remaining arguments (both command args and options)
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                config_.show_help = true;
                return;  // Return early, command is already set
            } else if (arg == "--version" || arg == "-v") {
                config_.show_version = true;
                return;
            } else if (arg == "--log-level" && i + 1 < argc) {
                config_.log_level = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                config_.port = std::stoi(argv[++i]);
            } else if (arg == "--host" && i + 1 < argc) {
                config_.host = argv[++i];
            } else if (arg == "--ctx-size" && i + 1 < argc) {
                config_.ctx_size = std::stoi(argv[++i]);
            } else if (arg == "--llamacpp" && i + 1 < argc) {
                config_.llamacpp_backend = argv[++i];
            } else if (arg == "--llamacpp-args" && i + 1 < argc) {
                config_.llamacpp_args = argv[++i];
            } else if (arg == "--max-loaded-models" && i + 1 < argc) {
                // Parse 1 or 3 values for max loaded models (2 or 4+ is not allowed)
                // All values must be positive integers (no floats, no negatives, no zero)
                std::vector<int> max_models;
                
                // Helper lambda to validate a string is a positive integer
                auto is_positive_integer = [](const std::string& s) -> bool {
                    if (s.empty()) return false;
                    for (char c : s) {
                        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                    }
                    return true;
                };
                
                // Parse all consecutive numeric values
                while (i + 1 < argc && argv[i + 1][0] != '-') {
                    std::string val_str = argv[++i];
                    if (!is_positive_integer(val_str)) {
                        std::cerr << "Error: --max-loaded-models values must be positive integers (got '" << val_str << "')" << std::endl;
                        exit(1);
                    }
                    int val = std::stoi(val_str);
                    if (val <= 0) {
                        std::cerr << "Error: --max-loaded-models values must be non-zero (got " << val << ")" << std::endl;
                        exit(1);
                    }
                    max_models.push_back(val);
                }
                
                // Validate: must have exactly 1, 3, or 4 values
                if (max_models.size() != 1 && max_models.size() != 3 && max_models.size() != 4) {
                    std::cerr << "Error: --max-loaded-models requires 1 value (LLMS), 3 values (LLMS EMBEDDINGS RERANKINGS), or 4 values (LLMS EMBEDDINGS RERANKINGS AUDIO), got " << max_models.size() << std::endl;
                    exit(1);
                }

                config_.max_llm_models = max_models[0];
                if (max_models.size() >= 3) {
                    config_.max_embedding_models = max_models[1];
                    config_.max_reranking_models = max_models[2];
                }
                if (max_models.size() == 4) {
                    config_.max_audio_models = max_models[3];
                }
            } else if (arg == "--no-tray") {
                config_.no_tray = true;
            } else {
                // It's a command argument (like model name)
                config_.command_args.push_back(arg);
            }
        }
        return;
    }
    
    // Check for global --help or --version flags (before command)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            config_.show_help = true;
            return;
        } else if (arg == "--version" || arg == "-v") {
            config_.show_version = true;
            return;
        }
    }
    
    // No command provided - this is an error
    if (argc == 1) {
        config_.command = "";  // Empty command signals error
        return;
    }
    
    // If we get here, we have flags but no command - also an error
    config_.command = "";
}

// ============================================================
// Help and Version Output
// ============================================================

void TrayApp::print_usage(bool show_serve_options) {
    std::cout << "lemonade-server - Lemonade Server\n\n";
    std::cout << "Usage: lemonade-server <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  serve                    Start the server\n";
    std::cout << "  run <model>              Run a model\n";
    std::cout << "  list                     List available models\n";
    std::cout << "  pull <model>             Download a model\n";
    std::cout << "  delete <model>           Delete a model\n";
    std::cout << "  status                   Check server status\n";
    std::cout << "  stop                     Stop the server\n\n";
    
    // Only show serve options if requested (for serve/run --help)
    if (show_serve_options) {
        std::cout << "Serve/Run Options:\n";
        std::cout << "  --port PORT              Server port (default: 8000)\n";
        std::cout << "  --host HOST              Server host (default: 127.0.0.1)\n";
        std::cout << "  --ctx-size SIZE          Context size (default: 4096)\n";
        std::cout << "  --llamacpp BACKEND       LlamaCpp backend: vulkan, rocm, metal, cpu (default: vulkan)\n";
        std::cout << "  --llamacpp-args ARGS     Custom arguments for llama-server\n";
        std::cout << "  --max-loaded-models N [E] [R] [A]\n";
        std::cout << "                           Max loaded models: LLMS [EMBEDDINGS] [RERANKINGS] [AUDIO] (default: 1 1 1 1)\n";
        std::cout << "  --log-file PATH          Log file path\n";
        std::cout << "  --log-level LEVEL        Log level: info, debug, trace (default: info)\n";
#if defined(__linux__) && !defined(__ANDROID__)
        std::cout << "  --no-tray                Start server without tray (default on Linux)\n";
#else
        std::cout << "  --no-tray                Start server without tray (headless mode)\n";
#endif
        std::cout << "\n";
    }
    
    std::cout << "  --help, -h               Show this help message\n";
    std::cout << "  --version, -v            Show version\n";
}

void TrayApp::print_version() {
    std::cout << "lemonade-server version " << current_version_ << std::endl;
}

void TrayApp::print_pull_help() {
    std::cout << "lemonade-server pull - Download and install a model\n\n";
    std::cout << "Usage:\n";
    std::cout << "  lemonade-server pull <model_name> [options]\n\n";
    std::cout << "Description:\n";
    std::cout << "  Downloads a model from the Lemonade Server registry or Hugging Face.\n";
    std::cout << "  For registered models, only the model name is required.\n";
    std::cout << "  For custom models, use the registration options below.\n\n";
    std::cout << "Registration Options (for custom models):\n";
    std::cout << "  --checkpoint CHECKPOINT  Hugging Face checkpoint (format: org/model:variant)\n";
    std::cout << "  --recipe RECIPE          Inference recipe to use\n";
    std::cout << "                           Options: llamacpp, flm, oga-cpu, oga-hybrid, oga-npu\n\n";
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
}

} // namespace lemon_server

