#include <iostream>
#include <csignal>
#include <atomic>
#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/server.h>
#include <lemon/version.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace lemon;

// Global flag for signal handling
static std::atomic<bool> g_shutdown_requested(false);
static Server* g_server_instance = nullptr;

// Signal handler for Ctrl+C, SIGTERM, and SIGHUP
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
#ifndef _WIN32
        const char* msg = "Shutdown signal received, exiting...\n";
        (void)write(STDOUT_FILENO, msg, 38);
#endif

        // Don't call server->stop() from signal handler - it can block/deadlock
        // Just set the flag and exit immediately. The OS will clean up resources.
        g_shutdown_requested = true;

        // Use _exit() for async-signal-safe immediate termination
        // The OS will handle cleanup of file descriptors, memory, and child processes
        _exit(0);
#ifdef SIGHUP
    } else if (signal == SIGHUP) {
        // Ignore SIGHUP to prevent termination when parent process exits
        // This allows the server to continue running as a daemon
        return;
#endif
    }
}

int main(int argc, char** argv) {
    try {
        CLIParser parser;
        parser.parse(argc, argv);

        if (!parser.should_continue()) {
            return parser.get_exit_code();
        }

        auto cli_config = parser.get_config();

        utils::set_home_dir(cli_config.home_dir);
        json config_json = ConfigFile::load(cli_config.home_dir);

        // CLI --port/--host override config.json and persist
        bool cli_overrides = false;
        if (cli_config.port != -1) {
            config_json["port"] = cli_config.port;
            cli_overrides = true;
        }
        if (!cli_config.host.empty()) {
            config_json["host"] = cli_config.host;
            cli_overrides = true;
        }
        if (cli_overrides) {
            ConfigFile::save(cli_config.home_dir, config_json);
        }

        auto config = std::make_shared<RuntimeConfig>(config_json);
        RuntimeConfig::set_global(config.get());

        auto sink = std::make_shared<AixLog::SinkCout>(
            AixLog::Filter(AixLog::to_severity(config->log_level())),
            RuntimeConfig::LOG_FORMAT);
        AixLog::Log::init({sink});

        utils::set_models_dir(config->models_dir());

        LOG(INFO) << "Starting Lemonade Server..." << std::endl;
        LOG(INFO) << "  Version: " << LEMON_VERSION_STRING << std::endl;
        LOG(INFO) << "  Home dir: " << cli_config.home_dir << std::endl;
        LOG(INFO) << "  Port: " << config->port() << std::endl;
        LOG(INFO) << "  Host: " << config->host() << std::endl;
        LOG(INFO) << "  Log level: " << config->log_level() << std::endl;
        if (!config->extra_models_dir().empty()) {
            LOG(INFO) << "  Extra models dir: " << config->extra_models_dir() << std::endl;
        }

        Server server(config, cli_config.home_dir);

        g_server_instance = &server;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        server.run();
        g_server_instance = nullptr;

        return 0;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Error: " << e.what() << std::endl;
        return 1;
    }
}
