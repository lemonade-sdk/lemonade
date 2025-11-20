#include <lemon/cli_parser.h>
#include <iostream>

namespace lemon {

CLIParser::CLIParser() 
    : app_("lemonade-router - Lightweight LLM server") {
    
    // Add version flag (help is automatically added by CLI11)
    app_.add_flag("-v,--version", show_version_, "Show version number");
    
    // Server options
    app_.add_option("--port", config_.port, "Port number to serve on")
        ->default_val(8000);
    
    app_.add_option("--host", config_.host, "Address to bind for connections")
        ->default_val("localhost");
    
    app_.add_option("--log-level", config_.log_level, "Log level for the server")
        ->check(CLI::IsMember({"critical", "error", "warning", "info", "debug", "trace"}))
        ->default_val("info");
    
    app_.add_option("--llamacpp", config_.llamacpp_backend, "LlamaCpp backend to use")
        ->check(CLI::IsMember({"vulkan", "rocm", "metal"}))
        ->default_val("vulkan");
    
    app_.add_option("--ctx-size", config_.ctx_size, "Context size for the model")
        ->default_val(4096);
    
    app_.add_option("--llamacpp-args", config_.llamacpp_args, 
                   "Custom arguments to pass to llama-server (must not conflict with managed args)")
        ->default_val("");
    
    // Multi-model support: Max loaded models
    // Use a member vector to capture 1-3 values
    app_.add_option("--max-loaded-models", max_models_vec_,
                   "Maximum number of models to keep loaded (format: LLMS [EMBEDDINGS] [RERANKINGS])")
        ->expected(1, 3);
}

int CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);
        
        // Process --max-loaded-models values
        if (!max_models_vec_.empty()) {
            config_.max_llm_models = max_models_vec_[0];
            if (max_models_vec_.size() > 1) {
                config_.max_embedding_models = max_models_vec_[1];
            }
            if (max_models_vec_.size() > 2) {
                config_.max_reranking_models = max_models_vec_[2];
            }
        }
        
        should_continue_ = true;
        exit_code_ = 0;
        return 0;  // Success, continue
    } catch (const CLI::ParseError& e) {
        // Help/version requested or parse error occurred
        // Let CLI11 handle printing and get the exit code
        exit_code_ = app_.exit(e);
        should_continue_ = false;  // Don't continue, just exit
        return exit_code_;
    }
}

} // namespace lemon
