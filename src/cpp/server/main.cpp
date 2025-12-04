#include <iostream>
#include <csignal>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/single_instance.h>
#include <lemon/version.h>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace lemon;

// Global flag for signal handling
static std::atomic<bool> g_shutdown_requested(false);
static Server* g_server_instance = nullptr;

// Signal handler for crashes - ensures we output something before dying
static void crash_signal_handler(int sig) {
    const char* sig_name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sig_name = "SIGSEGV (Segmentation fault)"; break;
        case SIGABRT: sig_name = "SIGABRT (Abort)"; break;
        case SIGFPE:  sig_name = "SIGFPE (Floating point exception)"; break;
        case SIGILL:  sig_name = "SIGILL (Illegal instruction)"; break;
#ifndef _WIN32
        case SIGBUS:  sig_name = "SIGBUS (Bus error)"; break;
#endif
    }
    
    // Use write() instead of std::cerr for async-signal-safety
    const char* prefix = "\nlemonade-router: Crashed with signal ";
    const char* suffix = "\nPlease report this issue at: https://github.com/aigdat/lemonade/issues\n";
#ifdef _WIN32
    _write(_fileno(stderr), prefix, strlen(prefix));
    _write(_fileno(stderr), sig_name, strlen(sig_name));
    _write(_fileno(stderr), suffix, strlen(suffix));
#else
    write(STDERR_FILENO, prefix, strlen(prefix));
    write(STDERR_FILENO, sig_name, strlen(sig_name));
    write(STDERR_FILENO, suffix, strlen(suffix));
#endif
    
    // Re-raise to get default behavior (core dump, etc.)
    signal(sig, SIG_DFL);
    raise(sig);
}

// Signal handler for Ctrl+C and SIGTERM
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
    }
}

int main(int argc, char** argv) {
    // CRITICAL: Disable buffering on stdout/stderr immediately
    // This ensures output appears even if the program crashes
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::ios_base::sync_with_stdio(true);
    
    // Install crash signal handlers to provide diagnostic output
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGFPE, crash_signal_handler);
    std::signal(SIGILL, crash_signal_handler);
#ifndef _WIN32
    std::signal(SIGBUS, crash_signal_handler);
#endif
    
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
            std::cout << "lemonade-router version " << LEMON_VERSION_STRING << std::endl;
            return 0;
        }
        
        // Get server configuration
        auto config = parser.get_config();
        
        // Start the server
        std::cout << "Starting Lemonade Server..." << std::endl;
        std::cout << "  Version: " << LEMON_VERSION_STRING << std::endl;
        std::cout << "  Port: " << config.port << std::endl;
        std::cout << "  Host: " << config.host << std::endl;
        std::cout << "  Log level: " << config.log_level << std::endl;
        std::cout << "  Context size: " << config.ctx_size << std::endl;
        
        Server server(config.port, config.host, config.log_level,
                    config.ctx_size, config.tray, config.llamacpp_backend,
                    config.llamacpp_args, config.max_llm_models,
                    config.max_embedding_models, config.max_reranking_models);
        
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
        std::cerr.flush();
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error occurred" << std::endl;
        std::cerr.flush();
        return 1;
    }
}
