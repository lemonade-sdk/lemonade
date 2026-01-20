#include <lemon/cli_parser.h>
#include <lemon/recipe_options.h>
#include <lemon/version.h>
#include <iostream>
#include <cctype>
#include <cstdlib>

#ifdef LEMONADE_TRAY
#define APP_NAME "lemonade-server"
#define APP_DESC APP_NAME " - Lemonade Server"
#else
#define APP_NAME "lemonade-router"
#define APP_DESC APP_NAME " - Lightweight LLM server"
#endif

namespace lemon {

static void add_serve_options(CLI::App* serve, ServerConfig& config, std::vector<int>& max_models_vec) {
    serve->add_option("--port", config.port, "Port number to serve on")
        ->envname("LEMONADE_PORT")
        ->default_val(8000);

    serve->add_option("--host", config.host, "Address to bind for connections")
        ->envname("LEMONADE_HOST")
        ->default_val("localhost");

    serve->add_option("--log-level", config.log_level, "Log level for the server")
        ->envname("LEMONADE_LOG_LEVEL")
        ->check(CLI::IsMember({"critical", "error", "warning", "info", "debug", "trace"}))
        ->default_val("info");

    serve->add_option("--extra-models-dir", config.extra_models_dir,
                   "Experimental feature: secondary directory to scan for LLM GGUF model files")
        ->envname("LEMONADE_EXTRA_MODELS_DIR")
        ->default_val("");

    // Multi-model support: Max loaded models
    // Use a member vector to capture 1, 3, or 4 values (2 is not allowed)
    serve->add_option("--max-loaded-models", max_models_vec,
                   "Maximum number of models to keep loaded (format: LLMS or LLMS EMBEDDINGS RERANKINGS [AUDIO])")
        ->expected(1, 4)
        ->check([](const std::string& val) -> std::string {
            // Validate that value is a positive integer (digits only, no floats)
            if (val.empty()) {
                return "Value must be a positive integer (got empty string)";
            }
            for (char c : val) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return "Value must be a positive integer (got '" + val + "')";
                }
            }
            try {
                int num = std::stoi(val);
                if (num <= 0) {
                    return "Value must be a non-zero positive integer (got " + val + ")";
                }
            } catch (...) {
                return "Value must be a positive integer (got '" + val + "')";
            }
            return "";  // Valid
        });
    RecipeOptions::add_cli_options(*serve, config.recipe_options);
}

CLIParser::CLIParser()
    : app_(APP_DESC) {

    app_.set_version_flag("-v,--version", (APP_NAME " version " LEMON_VERSION_STRING));

#ifdef LEMONADE_TRAY
    app_.require_subcommand(1);

    // Serve
    CLI::App* serve = app_.add_subcommand("serve", "Start the server");
    add_serve_options(serve, config_, max_models_vec_);
    serve->add_flag("--no-tray", tray_config_.no_tray, "Start server without tray (headless mode, default on Linux)");

    // Run
    CLI::App* run = app_.add_subcommand("run", "Run a model");
    run->add_option("model", tray_config_.model, "The model to run")->required();
    add_serve_options(run, config_, max_models_vec_);
    run->add_flag("--no-tray", tray_config_.no_tray, "Start server without tray (headless mode, default on Linux)");
    run->add_flag("--save-options", tray_config_.save_options, "Save model load options as default for this model");

    // List
    CLI::App* list = app_.add_subcommand("list", "List available models");

    // Pull
    CLI::App* pull = app_.add_subcommand("pull", "Download a model");
    pull->add_option("model", tray_config_.model, "The model to download")->required();
    pull->add_option("--checkpoint", tray_config_.checkpoint, "Hugging Face checkpoint (format: org/model:variant) OR an absolute local path to a model directory. When a local path is provided, files are copied to the HuggingFace cache and registered.");
    pull->add_option("--recipe", tray_config_.recipe, "Inference recipe to use. Required when using a local path.")
        ->check(CLI::IsMember({"llamacpp", "flm", "oga-cpu", "oga-hybrid", "oga-npu", "ryzenai", "whispercpp"}));
    pull->add_flag("--reasoning", tray_config_.is_reasoning, "Mark model as a reasoning model (e.g., DeepSeek-R1). Adds 'reasoning' label to model metadata.");
    pull->add_flag("--vision", tray_config_.is_vision, "Mark model as a vision model (multimodal). Adds 'vision' label to model metadata.");
    pull->add_flag("--embedding", tray_config_.is_embedding, "Mark model as an embedding model. Adds 'embeddings' label to model metadata. For use with /api/v1/embeddings endpoint.");
    pull->add_flag("--reranking", tray_config_.is_reranking, "Mark model as a reranking model. Adds 'reranking' label to model metadata. For use with /api/v1/reranking endpoint.");
    pull->add_option("--mmproj", tray_config_.mmproj, "Multimodal projector file for vision models. Required for GGUF vision models. Example: mmproj-model-f16.gguf");
     
    // Delete
    CLI::App* del = app_.add_subcommand("delete", "Delete a model");
    del->add_option("model", tray_config_.model, "The model to delete")->required();

    // Status
    CLI::App* status = app_.add_subcommand("status", "Check server status");

    // Stop
    CLI::App* stop = app_.add_subcommand("stop", "Stop the server");
#else
    add_serve_options(&app_, config_, max_models_vec_);
#endif    
}

int CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);

        // Process --max-loaded-models values
        if (!max_models_vec_.empty()) {
            // Validate that we have exactly 1, 3, or 4 values (2 is not allowed)
            if (max_models_vec_.size() == 2) {
                throw CLI::ValidationError("--max-loaded-models requires 1 value (LLMS), 3 values (LLMS EMBEDDINGS RERANKINGS), or 4 values (LLMS EMBEDDINGS RERANKINGS AUDIO), not 2");
            }

            config_.max_llm_models = max_models_vec_[0];
            if (max_models_vec_.size() >= 3) {
                config_.max_embedding_models = max_models_vec_[1];
                config_.max_reranking_models = max_models_vec_[2];
            }
            if (max_models_vec_.size() > 3) {
                config_.max_audio_models = max_models_vec_[3];
            }
        }
#ifdef LEMONADE_TRAY
        tray_config_.command = app_.get_subcommands().at(0)->get_name();
#endif
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
