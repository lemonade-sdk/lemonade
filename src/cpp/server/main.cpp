#include <iostream>
#include <csignal>
#include <atomic>
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/single_instance.h>
#include <lemon/version.h>

using namespace lemon;

// Global flag for signal handling
static std::atomic<bool> g_shutdown_requested(false);
static Server* g_server_instance = nullptr;

// Signal handler for Ctrl+C, SIGTERM, and SIGHUP
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[Server] Shutdown signal received, exiting..." << std::endl;
        std::cout.flush();

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
    // Check for single instance early (before parsing args for faster feedback)
    if (SingleInstance::IsAnotherInstanceRunning("Router")) {
        std::cerr << "Error: Another instance of lemonade-router is already running.\n"
                  << "Only one instance can run at a time.\n" << std::endl;
        return 1;
    }

    try {
        CLIParser parser;

        parser.parse(argc, argv);

        // Check if we should continue (false for --help, --version, or errors)
        if (!parser.should_continue()) {
            return parser.get_exit_code();
        }

        // Get server configuration
        auto config = parser.get_config();

        // Start the server
        std::cout << "Starting Lemonade Server..." << std::endl;
        std::cout << "  Version: " << LEMON_VERSION_STRING << std::endl;
        std::cout << "  Port: " << config.port << std::endl;
        std::cout << "  Host: " << config.host << std::endl;
        std::cout << "  Log level: " << config.log_level << std::endl;
        if (!config.extra_models_dir.empty()) {
            std::cout << "  Extra models dir: " << config.extra_models_dir << std::endl;
        }

        Server server(config.port, config.host, config.log_level,
                    config.recipe_options, config.max_llm_models,
                    config.max_embedding_models, config.max_reranking_models,
                    config.max_audio_models, config.max_image_models,
                    config.extra_models_dir, config.no_broadcast);

        // Register signal handler for Ctrl+C
        g_server_instance = &server;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        server.run();

        // Clean up
        g_server_instance = nullptr;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
