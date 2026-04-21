#include <lemon/cli_parser.h>
#include <lemon/utils/path_utils.h>
#include <lemon/version.h>

#define APP_NAME "lemond"
#define APP_DESC APP_NAME " - Lightweight LLM server"

namespace lemon {

CLIParser::CLIParser()
    : app_(APP_DESC) {

    app_.set_version_flag("-v,--version", (APP_NAME " version " LEMON_VERSION_STRING));

    // Positional arg: lemonade cache directory (optional)
    // Default to platform-specific cache dir when not specified
    app_.add_option("cache_dir", config_.cache_dir,
                    "Lemonade cache directory containing config.json and model data")
        ->type_name("DIR")
        ->default_val(utils::get_cache_dir());

    app_.add_option("--port", config_.port, "Port number to serve on (overrides config.json)")
        ->type_name("PORT");

    app_.add_option("--host", config_.host, "Address to bind for connections (overrides config.json)")
        ->type_name("HOST");

    // Llama.cpp router-mode flags. When set they override config.json and are
    // persisted on startup.
    app_.add_flag(
        "--router-mode,!--no-router-mode",
        config_.router_mode,
        "Launch a single llama-server in router mode for all llamacpp models");

    app_.add_option("--models-preset", config_.router_models_preset,
                    ".ini preset file passed to llama-server (router mode)")
        ->type_name("PATH");

    app_.add_option("--models-dir", config_.router_models_dir,
                    "GGUF directory passed to llama-server (router mode)")
        ->type_name("DIR");
}

int CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);
        // Record which CLI-only override flags were actually specified so the
        // caller knows whether to persist them over config.json.
        config_.router_mode_set = app_.count("--router-mode") > 0
                               || app_.count("--no-router-mode") > 0;
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
