#include <lemon/cli_parser.h>
#include <lemon/recipe_options.h>
#include <lemon/version.h>

#define APP_NAME "lemond"
#define APP_DESC APP_NAME " - Lightweight LLM server"

namespace lemon {

static void add_serve_options(CLI::App* serve, ServerConfig& config) {
    serve->add_option("--port", config.port, "Port number to serve on")
        ->envname("LEMONADE_PORT")
        ->type_name("PORT")
        ->default_val(config.port);

    serve->add_option("--host", config.host, "Address to bind for connections")
        ->envname("LEMONADE_HOST")
        ->type_name("HOST")
        ->default_val(config.host);

    serve->add_option("--websocket-port", config.websocket_port, "Port for the shared WebSocket server (0 = auto)")
        ->envname("LEMONADE_WEBSOCKET_PORT")
        ->type_name("PORT")
        ->default_val(config.websocket_port);

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

    serve->add_option("--global-timeout", config.global_timeout,
                   "Global timeout for HTTP requests, inference, and readiness checks in seconds")
        ->envname("LEMONADE_GLOBAL_TIMEOUT")
        ->type_name("SECONDS")
        ->default_val(config.global_timeout);

    // Multi-model support: Max loaded models per type slot
    serve->add_option("--max-loaded-models", config.max_loaded_models,
                   "Max models per type slot (LLMs, audio, image, etc.). Use -1 for unlimited.")
        ->envname("LEMONADE_MAX_LOADED_MODELS")
        ->type_name("N")
        ->default_val(config.max_loaded_models)
        ->check([](const std::string& val) -> std::string {
            try {
                int num = std::stoi(val);
                if (num == -1 || num > 0) {
                    return "";  // Valid: -1 (unlimited) or positive integer
                }
                return "Value must be a positive integer or -1 for unlimited (got " + val + ")";
            } catch (...) {
                return "Value must be a positive integer or -1 for unlimited (got '" + val + "')";
            }
        });
    RecipeOptions::add_cli_options(*serve, config.recipe_options);
}

CLIParser::CLIParser()
    : app_(APP_DESC) {

    app_.set_version_flag("-v,--version", (APP_NAME " version " LEMON_VERSION_STRING));
    add_serve_options(&app_, config_);
}

int CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);
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
