#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/logging_config.h>
#include <lemon/server.h>
#include <lemon/system_info.h>
#include <lemon/version.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace lemon;

// Global flags for signal handling
static std::atomic<bool> g_shutdown_requested(false);
static std::atomic<bool> g_reload_requested(false);
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
        // Set the reload flag; a background thread will call invalidate_recipes().
        // Calling mutex-based code directly from a signal handler is not async-signal-safe.
        g_reload_requested = true;
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

        // Initialize logging early with INFO so config loading messages are captured
        {
            auto early_filter = AixLog::Filter(AixLog::Severity::info);
            auto early_sink = std::make_shared<AixLog::SinkCout>(early_filter, RuntimeConfig::LOG_FORMAT);
            AixLog::Log::init({early_sink});
        }

        utils::set_cache_dir(cli_config.cache_dir);
        json config_json = ConfigFile::load(cli_config.cache_dir);

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

        // CLI router-mode flags override config.json and persist.
        if (cli_config.router_mode_set) {
            config_json["router_mode"] = cli_config.router_mode;
            cli_overrides = true;
        }
        if (!cli_config.router_models_preset.empty()) {
            config_json["router_models_preset"] = cli_config.router_models_preset;
            cli_overrides = true;
        }
        if (!cli_config.router_models_dir.empty()) {
            config_json["router_models_dir"] = cli_config.router_models_dir;
            cli_overrides = true;
        }

        auto config = std::make_shared<RuntimeConfig>(config_json);
        RuntimeConfig::set_global(config.get());

        // Initialize logging with the configured level — console + file + log hub
        configure_application_logging(config->log_level(), LoggingMode::direct_server);

        if (cli_overrides) {
            ConfigFile::save(cli_config.cache_dir, config_json);
            if (cli_config.port != -1) {
                LOG(INFO) << "Persisted port=" << cli_config.port << " to config.json" << std::endl;
            }
            if (!cli_config.host.empty()) {
                LOG(INFO) << "Persisted host=" << cli_config.host << " to config.json" << std::endl;
            }
            if (cli_config.router_mode_set) {
                LOG(INFO) << "Persisted router_mode=" << (cli_config.router_mode ? "true" : "false")
                          << " to config.json" << std::endl;
            }
            if (!cli_config.router_models_preset.empty()) {
                LOG(INFO) << "Persisted router_models_preset=" << cli_config.router_models_preset
                          << " to config.json" << std::endl;
            }
            if (!cli_config.router_models_dir.empty()) {
                LOG(INFO) << "Persisted router_models_dir=" << cli_config.router_models_dir
                          << " to config.json" << std::endl;
            }
        }

        utils::set_models_dir(config->models_dir());

        LOG(INFO) << "Starting Lemonade Server..." << std::endl;
        LOG(INFO) << "  Version: " << LEMON_VERSION_STRING << std::endl;
        LOG(INFO) << "  Cache dir: " << cli_config.cache_dir << std::endl;
        LOG(INFO) << "  Port: " << config->port() << std::endl;
        LOG(INFO) << "  Host: " << config->host() << std::endl;
        LOG(INFO) << "  Log level: " << config->log_level() << std::endl;
        if (!config->extra_models_dir().empty()) {
            LOG(INFO) << "  Extra models dir: " << config->extra_models_dir() << std::endl;
        }
        if (config->router_mode()) {
            LOG(INFO) << "  Router mode: enabled" << std::endl;
            if (!config->router_models_preset().empty()) {
                LOG(INFO) << "    models preset: " << config->router_models_preset() << std::endl;
            }
            if (!config->router_models_dir().empty()) {
                LOG(INFO) << "    models dir: " << config->router_models_dir() << std::endl;
            }
            if (config->router_models_preset().empty() && config->router_models_dir().empty()) {
                LOG(WARNING) << "    router mode has no model source configured; "
                             << "set --models-preset or --models-dir" << std::endl;
            }
        }

        Server server(config, cli_config.cache_dir);

        g_server_instance = &server;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#ifdef SIGHUP
        std::signal(SIGHUP, signal_handler);

        // Background thread: watches g_reload_requested and calls invalidate_recipes().
        // Mutex-based code (like invalidate_recipes) must not be called directly from
        // a signal handler, so we use this thread to do the actual work safely.
        std::thread([]() {
            while (!g_shutdown_requested.load()) {
                if (g_reload_requested.exchange(false)) {
                    LOG(INFO) << "SIGHUP received - rescanning hardware and recipes..." << std::endl;
                    SystemInfoCache::invalidate_recipes();
                    LOG(INFO) << "Hardware rescan complete" << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();
#endif

        server.run();
        g_server_instance = nullptr;

        return 0;

    } catch (const std::exception& e) {
        LOG(ERROR) << "Error: " << e.what() << std::endl;
        return 1;
    }
}
