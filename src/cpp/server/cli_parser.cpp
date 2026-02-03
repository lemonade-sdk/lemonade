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

#define PULL_FOOTER_COMMON "Examples:\n" \
    "  # Pull a registered model\n" \
    "  lemonade-server pull Llama-3.2-1B-Instruct-GGUF\n\n" \
    "  # Pull from HuggingFace with custom name\n" \
    "  lemonade-server pull user.MyLlama --checkpoint meta-llama/Llama-3.2-1B-Instruct-GGUF:Q4_K_M --recipe llamacpp\n\n" \
    "  # Import from local directory\n"
#ifdef _WIN32
    #define PULL_FOOTER PULL_FOOTER_COMMON "  lemonade-server pull user.MyModel --checkpoint C:\\models\\my-model --recipe llamacpp"
#else
    #define PULL_FOOTER PULL_FOOTER_COMMON "  lemonade-server pull user.MyModel --checkpoint /home/user/models/my-model --recipe llamacpp"
#endif


namespace lemon {

static void add_serve_options(CLI::App* serve, ServerConfig& config, std::vector<int>& max_models_vec) {
    serve->add_option("--port", config.port, "Port number to serve on")
        ->envname("LEMONADE_PORT")
        ->type_name("PORT")
        ->default_val(config.port);

    serve->add_option("--host", config.host, "Address to bind for connections")
        ->envname("LEMONADE_HOST")
        ->type_name("HOST")
        ->default_val(config.host);

    serve->add_option("--log-level", config.log_level, "Log level for the server")
        ->envname("LEMONADE_LOG_LEVEL")
        ->type_name("LEVEL")
        ->check(CLI::IsMember({"critical", "error", "warning", "info", "debug", "trace"}))
        ->default_val(config.log_level);

    serve->add_option("--extra-models-dir", config.extra_models_dir,
                   "Experimental feature: secondary directory to scan for LLM GGUF model files")
        ->envname("LEMONADE_EXTRA_MODELS_DIR")
        ->type_name("PATH")
        ->default_val(config.extra_models_dir);

    serve->add_flag("--no-broadcast", config.no_broadcast,
                   "Disable UDP broadcasting on private networks")
        ->envname("LEMONADE_NO_BROADCAST")
        ->expected(0, 1)
        ->default_val(config.no_broadcast);

    // Multi-model support: Max loaded models
    // Use a member vector to capture 1, 3, 4, or 5 values (2 is not allowed)
    serve->add_option("--max-loaded-models", max_models_vec,
                   "Max loaded models: LLMS [EMBEDDINGS] [RERANKINGS] [AUDIO] [IMAGE]")
        ->type_name("N [E] [R] [A] [I]")
        ->expected(1, 5)
        ->default_val(std::vector<int>{config.max_llm_models, config.max_embedding_models, config.max_reranking_models, config.max_audio_models, config.max_image_models})
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
    app_.set_help_all_flag("--help-all", "Print help for all commands");

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
    pull->add_option("model", tray_config_.model, "The model to download")
        ->type_name("MODEL")
        ->required();
    pull->add_option("--checkpoint", tray_config_.checkpoint, "Hugging Face checkpoint (format: org/model:variant) OR an absolute local path to a model directory. When a local path is provided, files are copied to the HuggingFace cache and registered.")
        ->type_name("CHECKPOINT");
    pull->add_option("--recipe", tray_config_.recipe, "Inference recipe to use. Required when using a local path.")
        ->type_name("RECIPE")
        ->check(CLI::IsMember({"llamacpp", "flm", "oga-cpu", "oga-hybrid", "oga-npu", "ryzenai", "whispercpp"}));
    pull->add_flag("--reasoning", tray_config_.is_reasoning, "Mark model as a reasoning model (e.g., DeepSeek-R1). Adds 'reasoning' label to model metadata.");
    pull->add_flag("--vision", tray_config_.is_vision, "Mark model as a vision model (multimodal). Adds 'vision' label to model metadata.");
    pull->add_flag("--embedding", tray_config_.is_embedding, "Mark model as an embedding model. Adds 'embeddings' label to model metadata. For use with /api/v1/embeddings endpoint.");
    pull->add_flag("--reranking", tray_config_.is_reranking, "Mark model as a reranking model. Adds 'reranking' label to model metadata. For use with /api/v1/reranking endpoint.");
    pull->add_option("--mmproj", tray_config_.mmproj, "Multimodal projector file for vision models. Required for GGUF vision models. Example: mmproj-model-f16.gguf")
        ->type_name("FILENAME");
    pull->footer(PULL_FOOTER);

    // Delete
    CLI::App* del = app_.add_subcommand("delete", "Delete a model");
    del->add_option("model", tray_config_.model, "The model to delete")->required();

    // Status
    CLI::App* status = app_.add_subcommand("status", "Check server status");

    // Stop
    CLI::App* stop = app_.add_subcommand("stop", "Stop the server");

    // Recipes
    CLI::App* recipes = app_.add_subcommand("recipes", "List execution backends");
#else
    add_serve_options(&app_, config_, max_models_vec_);
#endif
}

int CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);

        // Process --max-loaded-models values
        if (!max_models_vec_.empty()) {
            // Validate that we have exactly 1, 3, 4, or 5 values (2 is not allowed)
            if (max_models_vec_.size() == 2) {
                throw CLI::ValidationError("--max-loaded-models requires 1 value (LLMS), 3 values (LLMS EMBEDDINGS RERANKINGS), 4 values (LLMS EMBEDDINGS RERANKINGS AUDIO), or 5 values (LLMS EMBEDDINGS RERANKINGS AUDIO IMAGE), not 2");
            }

            config_.max_llm_models = max_models_vec_[0];
            if (max_models_vec_.size() >= 3) {
                config_.max_embedding_models = max_models_vec_[1];
                config_.max_reranking_models = max_models_vec_[2];
            }
            if (max_models_vec_.size() > 3) {
                config_.max_audio_models = max_models_vec_[3];
            }
            if (max_models_vec_.size() > 4) {
                config_.max_image_models = max_models_vec_[4];
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
