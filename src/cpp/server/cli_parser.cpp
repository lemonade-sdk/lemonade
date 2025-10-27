#include <lemon/cli_parser.h>
#include <iostream>

namespace lemon {

CLIParser::CLIParser() 
    : app_("lemon.cpp - Lightweight LLM server") {
    
    app_.require_subcommand(0, 1);
    app_.fallthrough(false);
    
    // Add version flag
    app_.add_flag("-v,--version", show_version_, "Show version number");
    
    // Setup all subcommands
    setup_serve_command();
    setup_status_command();
    setup_stop_command();
    setup_list_command();
    setup_pull_command();
    setup_delete_command();
    setup_run_command();
}

bool CLIParser::parse(int argc, char** argv) {
    try {
        app_.parse(argc, argv);
        return true;
    } catch (const CLI::ParseError& e) {
        return app_.exit(e) == 0;
    }
}

void CLIParser::setup_serve_command() {
    auto* serve = app_.add_subcommand("serve", "Start the server");
    
    serve->add_option("--port", serve_config_.port, "Port number to serve on")
        ->default_val(8000);
    
    serve->add_option("--host", serve_config_.host, "Address to bind for connections")
        ->default_val("0.0.0.0");
    
    serve->add_option("--log-level", serve_config_.log_level, "Log level for the server")
        ->check(CLI::IsMember({"critical", "error", "warning", "info", "debug", "trace"}))
        ->default_val("info");
    
#ifdef _WIN32
    serve->add_flag("--no-tray,!--tray", serve_config_.tray, 
                   "Do not show a tray icon when the server is running");
#endif
    
    serve->add_option("--llamacpp", serve_config_.llamacpp_backend, "LlamaCpp backend to use")
        ->check(CLI::IsMember({"vulkan", "rocm", "metal"}))
        ->default_val("vulkan");
    
    serve->add_option("--ctx-size", serve_config_.ctx_size, "Context size for the model")
        ->default_val(4096);
    
    serve->callback([this]() { command_ = "serve"; });
}

void CLIParser::setup_status_command() {
    auto* status = app_.add_subcommand("status", "Check if server is running");
    status->callback([this]() { command_ = "status"; });
}

void CLIParser::setup_stop_command() {
    auto* stop = app_.add_subcommand("stop", "Stop the server");
    stop->callback([this]() { command_ = "stop"; });
}

void CLIParser::setup_list_command() {
    auto* list = app_.add_subcommand("list", "List recommended models and their download status");
    list->callback([this]() { command_ = "list"; });
}

void CLIParser::setup_pull_command() {
    auto* pull = app_.add_subcommand("pull", "Install an LLM");
    
    pull->add_option("model", pull_config_.models, "Lemonade Server model name")
        ->required();
    
    pull->add_option("--checkpoint", pull_config_.checkpoint,
                    "For registering a new model: Hugging Face checkpoint to source the model from");
    
    pull->add_option("--recipe", pull_config_.recipe,
                    "For registering a new model: lemonade.api recipe to use with the model");
    
    pull->add_flag("--reasoning", pull_config_.reasoning,
                  "For registering a new model: whether the model is a reasoning model or not");
    
    pull->add_flag("--vision", pull_config_.vision,
                  "For registering a new model: whether the model has vision capabilities");
    
    pull->add_option("--mmproj", pull_config_.mmproj,
                    "For registering a new multimodal model: full file name of the .mmproj file");
    
    pull->callback([this]() { command_ = "pull"; });
}

void CLIParser::setup_delete_command() {
    auto* del = app_.add_subcommand("delete", "Delete an LLM");
    
    del->add_option("model", delete_config_.models, "Lemonade Server model name")
        ->required();
    
    del->callback([this]() { command_ = "delete"; });
}

void CLIParser::setup_run_command() {
    auto* run = app_.add_subcommand("run", "Chat with specified model (starts server if needed)");
    
    run->add_option("model", run_config_.model, "Lemonade Server model name to run")
        ->required();
    
    run->add_option("--port", run_config_.port, "Port number to serve on")
        ->default_val(8000);
    
    run->add_option("--host", run_config_.host, "Address to bind for connections")
        ->default_val("localhost");
    
    run->add_option("--log-level", run_config_.log_level, "Log level for the server")
        ->check(CLI::IsMember({"critical", "error", "warning", "info", "debug", "trace"}))
        ->default_val("info");
    
#ifdef _WIN32
    run->add_flag("--no-tray,!--tray", run_config_.tray,
                 "Do not show a tray icon when the server is running");
#endif
    
    run->add_option("--llamacpp", run_config_.llamacpp_backend, "LlamaCpp backend to use")
        ->check(CLI::IsMember({"vulkan", "rocm", "metal"}))
        ->default_val("vulkan");
    
    run->add_option("--ctx-size", run_config_.ctx_size, "Context size for the model")
        ->default_val(4096);
    
    run->callback([this]() { command_ = "run"; });
}

} // namespace lemon

