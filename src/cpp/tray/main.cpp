#include "lemon_tray/tray_app.h"
#include <iostream>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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
    const char* prefix = "\nlemonade-server: Crashed with signal ";
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

// Console entry point
// This is the CLI client - perfect for terminal use
int main(int argc, char* argv[]) {
    // CRITICAL: Disable buffering on stdout/stderr immediately
    // This ensures output appears even if the program crashes
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::ios_base::sync_with_stdio(true);
    
    // Install crash signal handlers to provide diagnostic output
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGFPE, crash_signal_handler);
    signal(SIGILL, crash_signal_handler);
#ifndef _WIN32
    signal(SIGBUS, crash_signal_handler);
#endif
    
    // Note: Single-instance check moved to serve command specifically
    // This allows status, list, pull, delete, stop to run while server is active
    
    try {
        lemon_tray::TrayApp app(argc, argv);
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        std::cerr.flush();
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;
        std::cerr.flush();
        return 1;
    }
}

