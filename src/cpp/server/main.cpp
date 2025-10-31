#include <iostream>
#include <csignal>
#include <atomic>
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/single_instance.h>

using namespace lemon;

// Global flag for signal handling
static std::atomic<bool> g_shutdown_requested(false);
static Server* g_server_instance = nullptr;

// Signal handler for Ctrl+C
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[Server] Shutdown signal received, cleaning up..." << std::endl;
        g_shutdown_requested = true;
        if (g_server_instance) {
            g_server_instance->stop();
        }
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
        
        if (parser.should_show_version()) {
            std::cout << "lemonade-router version 1.0.0" << std::endl;
            return 0;
        }
        
        // Get server configuration
        auto config = parser.get_config();
        
        // Start the server
        std::cout << "Starting Lemonade Server..." << std::endl;
        std::cout << "  Port: " << config.port << std::endl;
        std::cout << "  Host: " << config.host << std::endl;
        std::cout << "  Log level: " << config.log_level << std::endl;
        std::cout << "  Context size: " << config.ctx_size << std::endl;
        
        Server server(config.port, config.host, config.log_level,
                    config.ctx_size, config.tray, config.llamacpp_backend);
        
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
