#include "lemon_tray/tray_app.h"
#include "lemon_tray/platform/windows_tray.h"  // For set_menu_update_callback
#include <lemon/single_instance.h>
#include <lemon/version.h>
#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cctype>
#include <vector>
#include <set>

#ifdef _WIN32
#include <winsock2.h>  // Must come before windows.h
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <cstdlib>
#include <cstring>     // for strerror
#include <unistd.h>  // for readlink
#include <sys/wait.h>  // for waitpid
#include <sys/file.h>  // for flock
#include <fcntl.h>     // for open
#include <cerrno>      // for errno
#endif

namespace fs = std::filesystem;

namespace lemon_tray {

// Helper macro for debug logging
#define DEBUG_LOG(app, msg) \
    if ((app)->config_.log_level == "debug") { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

#ifndef _WIN32
// Initialize static signal pipe
int TrayApp::signal_pipe_[2] = {-1, -1};
#endif

#ifdef _WIN32
// Helper function to show a simple Windows notification without tray
static void show_simple_notification(const std::string& title, const std::string& message) {
    // Convert UTF-8 to wide string
    auto utf8_to_wstring = [](const std::string& str) -> std::wstring {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size_needed);
        if (!result.empty() && result.back() == L'\0') {
            result.pop_back();
        }
        return result;
    };
    
    // Create a temporary window class and window for the notification
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"LemonadeNotifyClass";
    RegisterClassW(&wc);
    
    HWND hwnd = CreateWindowW(L"LemonadeNotifyClass", L"", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    
    if (hwnd) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO | NIF_ICON;
        nid.dwInfoFlags = NIIF_INFO;
        
        // Use default icon
        nid.hIcon = LoadIcon(nullptr, IDI_INFORMATION);
        
        std::wstring title_wide = utf8_to_wstring(title);
        std::wstring message_wide = utf8_to_wstring(message);
        
        wcsncpy_s(nid.szInfoTitle, title_wide.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, message_wide.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szTip, L"Lemonade Server", _TRUNCATE);
        
        // Add the icon and show notification
        Shell_NotifyIconW(NIM_ADD, &nid);
        
        // Keep it displayed briefly then clean up
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Shell_NotifyIconW(NIM_DELETE, &nid);
        
        DestroyWindow(hwnd);
    }
    UnregisterClassW(L"LemonadeNotifyClass", GetModuleHandle(nullptr));
}
#endif

// Global pointer to the current TrayApp instance for signal handling
static TrayApp* g_tray_app_instance = nullptr;

#ifdef _WIN32
// Windows Ctrl+C handler
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        std::cout << "\nReceived interrupt signal, shutting down gracefully..." << std::endl;
        std::cout.flush();
        
        if (g_tray_app_instance) {
            g_tray_app_instance->shutdown();
        }
        
        // Exit the process explicitly to ensure cleanup completes
        // Windows will wait for this handler to return before terminating
        std::exit(0);
    }
    return FALSE;
}
#else
// Unix signal handler for SIGINT/SIGTERM
void signal_handler(int signal) {
    if (signal == SIGINT) {
        // SIGINT = User pressed Ctrl+C
        // We MUST clean up children ourselves
        // Write to pipe - main thread will handle cleanup
        // write() is async-signal-safe
        char sig = (char)signal;
        ssize_t written = write(TrayApp::signal_pipe_[1], &sig, 1);
        (void)written;  // Suppress unused variable warning
        
    } else if (signal == SIGTERM) {
        // SIGTERM = Stop command is killing us
        // Stop command will handle killing children
        // Just exit immediately to avoid race condition
        std::cout << "\nReceived termination signal, exiting..." << std::endl;
        std::cout.flush();
        _exit(0);
    }
}

// SIGCHLD handler to automatically reap zombie children
void sigchld_handler(int signal) {
    // Reap all zombie children without blocking
    // This prevents the router process from becoming a zombie
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        // Child reaped successfully
    }
}

// Helper function to check if a process is alive (and not a zombie)
static bool is_process_alive_not_zombie(pid_t pid) {
    if (pid <= 0) return false;
    
    // First check if process exists at all
    if (kill(pid, 0) != 0) {
        return false;  // Process doesn't exist
    }
    
    // Check if it's a zombie by reading /proc/PID/stat
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);
    if (!stat_file) {
        return false;  // Can't read stat, assume dead
    }
    
    std::string line;
    std::getline(stat_file, line);
    
    // Find the state character (after the closing paren of the process name)
    size_t paren_pos = line.rfind(')');
    if (paren_pos != std::string::npos && paren_pos + 2 < line.length()) {
        char state = line[paren_pos + 2];
        // Return false if zombie
        return (state != 'Z');
    }
    
    // If we can't parse the state, assume alive to be safe
    return true;
}
#endif

TrayApp::TrayApp(int argc, char* argv[])
    : current_version_(LEMON_VERSION_STRING)
    , should_exit_(false)
{
    // Load defaults from environment variables before parsing command-line arguments
    load_env_defaults();
    parse_arguments(argc, argv);
    
    if (config_.show_help) {
        // Show command-specific help
        if (config_.command == "pull") {
            print_pull_help();
        } else {
            // Show serve options only if command is "serve" or "run"
            bool show_serve_options = (config_.command == "serve" || config_.command == "run");
            print_usage(show_serve_options);
        }
        exit(0);
    }
    
    if (config_.show_version) {
        print_version();
        exit(0);
    }
    
    // Only set up signal handlers if we're actually going to run a command
    // (not for help/version which exit immediately)
    if (!config_.command.empty()) {
        g_tray_app_instance = this;
        
#ifdef _WIN32
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
        // Create self-pipe for safe signal handling
        if (pipe(signal_pipe_) == -1) {
            std::cerr << "Failed to create signal pipe: " << strerror(errno) << std::endl;
            exit(1);
        }
        
        // Set write end to non-blocking to prevent signal handler from blocking
        int flags = fcntl(signal_pipe_[1], F_GETFL);
        if (flags != -1) {
            fcntl(signal_pipe_[1], F_SETFL, flags | O_NONBLOCK);
        }
        
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        // Install SIGCHLD handler to automatically reap zombie children
        // This prevents the router process from becoming a zombie when it exits
        signal(SIGCHLD, sigchld_handler);
#endif
        
        DEBUG_LOG(this, "Signal handlers installed");
    }
}

TrayApp::~TrayApp() {
    // Stop signal monitor thread if running
#ifndef _WIN32
    if (signal_monitor_thread_.joinable()) {
        stop_signal_monitor_ = true;
        signal_monitor_thread_.join();
    }
#endif
    
    // Only shutdown if we actually started something
    if (server_manager_ || !config_.command.empty()) {
        shutdown();
    }
    
#ifndef _WIN32
    // Clean up signal pipe
    if (signal_pipe_[0] != -1) {
        close(signal_pipe_[0]);
        close(signal_pipe_[1]);
        signal_pipe_[0] = signal_pipe_[1] = -1;
    }
#endif
    
    g_tray_app_instance = nullptr;
}

int TrayApp::run() {
    // Check if no command was provided
    if (config_.command.empty()) {
        std::cerr << "Error: No command specified\n" << std::endl;
        print_usage();
        return 1;
    }
    
    DEBUG_LOG(this, "TrayApp::run() starting...");
    DEBUG_LOG(this, "Command: " << config_.command);
    
    // Find server binary automatically (needed for most commands)
    if (config_.server_binary.empty()) {
        DEBUG_LOG(this, "Searching for server binary...");
        if (!find_server_binary()) {
            std::cerr << "Error: Could not find lemonade-router binary" << std::endl;
#ifdef _WIN32
            std::cerr << "Please ensure lemonade-router.exe is in the same directory" << std::endl;
#else
            std::cerr << "Please ensure lemonade-router is in the same directory or in PATH" << std::endl;
#endif
            return 1;
        }
    }
    
    DEBUG_LOG(this, "Using server binary: " << config_.server_binary);
    
    // Handle commands
    if (config_.command == "list") {
        return execute_list_command();
    } else if (config_.command == "pull") {
        return execute_pull_command();
    } else if (config_.command == "delete") {
        return execute_delete_command();
    } else if (config_.command == "status") {
        return execute_status_command();
    } else if (config_.command == "stop") {
        return execute_stop_command();
    } else if (config_.command == "serve" || config_.command == "run") {
        // Check for single instance - only for 'serve' and 'run' commands
        // Other commands (status, list, pull, delete, stop) can run alongside a server
        if (lemon::SingleInstance::IsAnotherInstanceRunning("Server")) {
            // If 'run' command and server is already running, connect to it and execute the run command
            if (config_.command == "run") {
                std::cout << "Lemonade Server is already running. Connecting to it..." << std::endl;
                
                // Get the running server's info
                auto [pid, running_port] = get_server_info();
                if (running_port == 0) {
                    std::cerr << "Error: Could not connect to running server" << std::endl;
                    return 1;
                }
                
                // Create server manager to communicate with running server
                server_manager_ = std::make_unique<ServerManager>();
                server_manager_->set_port(running_port);
                config_.port = running_port;  // Update config to match running server
                
                // Use localhost to connect (works regardless of what the server is bound to)
                if (config_.host.empty() || config_.host == "0.0.0.0") {
                    config_.host = "localhost";
                }
                
                // Execute the run command (load model and open browser)
                return execute_run_command();
            }
            
            // For 'serve' command, don't allow duplicate servers
#ifdef _WIN32
            show_simple_notification("Server Already Running", "Lemonade Server is already running");
#endif
            std::cerr << "Error: Another instance of lemonade-server serve is already running.\n"
                      << "Only one persistent server can run at a time.\n\n"
                      << "To check server status: lemonade-server status\n"
                      << "To stop the server: lemonade-server stop\n" << std::endl;
            return 1;
        }
        // Continue to server initialization below
    } else {
        std::cerr << "Error: Unknown command '" << config_.command << "'\n" << std::endl;
        print_usage();
        return 1;
    }
    
    // Create server manager
    DEBUG_LOG(this, "Creating server manager...");
    server_manager_ = std::make_unique<ServerManager>();
    
    // Start server
    DEBUG_LOG(this, "Starting server...");
    if (!start_server()) {
        std::cerr << "Error: Failed to start server" << std::endl;
        return 1;
    }
    
    DEBUG_LOG(this, "Server started successfully!");
    
    // If this is the 'run' command, load the model and open browser
    if (config_.command == "run") {
        int result = execute_run_command();
        if (result != 0) {
            return result;
        }
    }
    
    // If no-tray mode, just wait for server to exit
    if (config_.no_tray) {
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
#ifdef _WIN32
        // Windows: simple sleep loop (signal handler handles Ctrl+C via console_ctrl_handler)
        while (server_manager_->is_server_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#else
        // Linux: monitor signal pipe using select() for proper signal handling
        while (server_manager_->is_server_running()) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(signal_pipe_[0], &readfds);
            
            struct timeval tv = {1, 0};  // 1 second timeout
            int result = select(signal_pipe_[0] + 1, &readfds, nullptr, nullptr, &tv);
            
            if (result > 0 && FD_ISSET(signal_pipe_[0], &readfds)) {
                // Signal received (SIGINT from Ctrl+C)
                char sig;
                ssize_t bytes_read = read(signal_pipe_[0], &sig, 1);
                (void)bytes_read;  // Suppress unused variable warning
                
                std::cout << "\nReceived interrupt signal, shutting down..." << std::endl;
                
                // Now we're safely in the main thread - call shutdown properly
                shutdown();
                break;
            }
            // Timeout or error - just continue checking if server is still running
        }
#endif
        
        return 0;
    }
    
    // Create tray application
    tray_ = create_tray();
    if (!tray_) {
        std::cerr << "Error: Failed to create tray for this platform" << std::endl;
        return 1;
    }
    
    DEBUG_LOG(this, "Tray created successfully");
    
    // Set log level for the tray
    tray_->set_log_level(config_.log_level);
    
    // Set ready callback
    DEBUG_LOG(this, "Setting ready callback...");
    tray_->set_ready_callback([this]() {
        DEBUG_LOG(this, "Ready callback triggered!");
        show_notification("Woohoo!", "Lemonade Server is running! Right-click the tray icon to access options.");
    });
    
    // Set menu update callback to refresh state before showing menu (Windows only)
    DEBUG_LOG(this, "Setting menu update callback...");
#ifdef _WIN32
    if (auto* windows_tray = dynamic_cast<WindowsTray*>(tray_.get())) {
        windows_tray->set_menu_update_callback([this]() {
            DEBUG_LOG(this, "Refreshing menu state from server...");
            build_menu();
        });
    }
#endif
    
    // Find icon path (matching the CMake resources structure)
    DEBUG_LOG(this, "Searching for icon...");
    std::string icon_path = "resources/static/favicon.ico";
    DEBUG_LOG(this, "Checking icon at: " << fs::absolute(icon_path).string());
    
    if (!fs::exists(icon_path)) {
        // Try relative to executable directory
        fs::path exe_path = fs::path(config_.server_binary).parent_path();
        icon_path = (exe_path / "resources" / "static" / "favicon.ico").string();
        DEBUG_LOG(this, "Icon not found, trying: " << icon_path);
        
        // If still not found, try without static subdir (fallback)
        if (!fs::exists(icon_path)) {
            icon_path = (exe_path / "resources" / "favicon.ico").string();
            DEBUG_LOG(this, "Icon not found, trying fallback: " << icon_path);
        }
    }
    
    if (fs::exists(icon_path)) {
        DEBUG_LOG(this, "Icon found at: " << icon_path);
    } else {
        std::cout << "WARNING: Icon not found at any location, will use default icon" << std::endl;
    }
    
    // Initialize tray
    DEBUG_LOG(this, "Initializing tray with icon: " << icon_path);
    if (!tray_->initialize("Lemonade Server", icon_path)) {
        std::cerr << "Error: Failed to initialize tray" << std::endl;
        return 1;
    }
    
    DEBUG_LOG(this, "Tray initialized successfully");
    
    // Build initial menu
    DEBUG_LOG(this, "Building menu...");
    build_menu();
    DEBUG_LOG(this, "Menu built successfully");
    
#ifndef _WIN32
    // On Linux, start a background thread to monitor the signal pipe
    // This allows us to handle Ctrl+C cleanly even when tray is running
    DEBUG_LOG(this, "Starting signal monitor thread...");
    signal_monitor_thread_ = std::thread([this]() {
        while (!stop_signal_monitor_ && !should_exit_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(signal_pipe_[0], &readfds);
            
            struct timeval tv = {0, 100000};  // 100ms timeout
            int result = select(signal_pipe_[0] + 1, &readfds, nullptr, nullptr, &tv);
            
            if (result > 0 && FD_ISSET(signal_pipe_[0], &readfds)) {
                // Signal received (SIGINT from Ctrl+C)
                char sig;
                ssize_t bytes_read = read(signal_pipe_[0], &sig, 1);
                (void)bytes_read;  // Suppress unused variable warning
                
                std::cout << "\nReceived interrupt signal, shutting down..." << std::endl;
                
                // Call shutdown from this thread (not signal context, so it's safe)
                shutdown();
                break;
            }
        }
        DEBUG_LOG(this, "Signal monitor thread exiting");
    });
#endif
    
    DEBUG_LOG(this, "Menu built, entering event loop...");
    // Run tray event loop
    tray_->run();
    
    DEBUG_LOG(this, "Event loop exited");
    return 0;
}

void TrayApp::load_env_defaults() {
    // Helper to get environment variable with fallback
    auto getenv_or_default = [](const char* name, const std::string& default_val) -> std::string {
        const char* val = std::getenv(name);
        return val ? std::string(val) : default_val;
    };
    
    // Helper to get integer environment variable with fallback
    auto getenv_int_or_default = [](const char* name, int default_val) -> int {
        const char* val = std::getenv(name);
        if (val) {
            try {
                return std::stoi(val);
            } catch (...) {
                // Invalid integer, use default
                return default_val;
            }
        }
        return default_val;
    };
    
    // Load environment variables into config (can be overridden by command-line args)
    config_.port = getenv_int_or_default("LEMONADE_PORT", config_.port);
    config_.host = getenv_or_default("LEMONADE_HOST", config_.host);
    config_.log_level = getenv_or_default("LEMONADE_LOG_LEVEL", config_.log_level);
    config_.llamacpp_backend = getenv_or_default("LEMONADE_LLAMACPP", config_.llamacpp_backend);
    config_.ctx_size = getenv_int_or_default("LEMONADE_CTX_SIZE", config_.ctx_size);
    config_.llamacpp_args = getenv_or_default("LEMONADE_LLAMACPP_ARGS", config_.llamacpp_args);
}

void TrayApp::parse_arguments(int argc, char* argv[]) {
    // Check if there's a command (non-flag argument)
    if (argc > 1 && argv[1][0] != '-') {
        config_.command = argv[1];
        
        // Parse remaining arguments (both command args and options)
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                config_.show_help = true;
                return;  // Return early, command is already set
            } else if (arg == "--version" || arg == "-v") {
                config_.show_version = true;
                return;
            } else if (arg == "--log-level" && i + 1 < argc) {
                config_.log_level = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                config_.port = std::stoi(argv[++i]);
            } else if (arg == "--host" && i + 1 < argc) {
                config_.host = argv[++i];
            } else if (arg == "--ctx-size" && i + 1 < argc) {
                config_.ctx_size = std::stoi(argv[++i]);
            } else if (arg == "--llamacpp" && i + 1 < argc) {
                config_.llamacpp_backend = argv[++i];
            } else if (arg == "--llamacpp-args" && i + 1 < argc) {
                config_.llamacpp_args = argv[++i];
            } else if (arg == "--max-loaded-models" && i + 1 < argc) {
                // Parse 1 or 3 values for max loaded models (2 or 4+ is not allowed)
                // All values must be positive integers (no floats, no negatives, no zero)
                std::vector<int> max_models;
                
                // Helper lambda to validate a string is a positive integer
                auto is_positive_integer = [](const std::string& s) -> bool {
                    if (s.empty()) return false;
                    for (char c : s) {
                        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                    }
                    return true;
                };
                
                // Parse all consecutive numeric values
                while (i + 1 < argc && argv[i + 1][0] != '-') {
                    std::string val_str = argv[++i];
                    if (!is_positive_integer(val_str)) {
                        std::cerr << "Error: --max-loaded-models values must be positive integers (got '" << val_str << "')" << std::endl;
                        exit(1);
                    }
                    int val = std::stoi(val_str);
                    if (val <= 0) {
                        std::cerr << "Error: --max-loaded-models values must be non-zero (got " << val << ")" << std::endl;
                        exit(1);
                    }
                    max_models.push_back(val);
                }
                
                // Validate: must have exactly 1 or 3 values
                if (max_models.size() != 1 && max_models.size() != 3) {
                    std::cerr << "Error: --max-loaded-models requires 1 value (LLMS) or 3 values (LLMS EMBEDDINGS RERANKINGS), got " << max_models.size() << std::endl;
                    exit(1);
                }
                
                config_.max_llm_models = max_models[0];
                if (max_models.size() == 3) {
                    config_.max_embedding_models = max_models[1];
                    config_.max_reranking_models = max_models[2];
                }
            } else if (arg == "--no-tray") {
                config_.no_tray = true;
            } else {
                // It's a command argument (like model name)
                config_.command_args.push_back(arg);
            }
        }
        return;
    }
    
    // Check for global --help or --version flags (before command)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            config_.show_help = true;
            return;
        } else if (arg == "--version" || arg == "-v") {
            config_.show_version = true;
            return;
        }
    }
    
    // No command provided - this is an error
    if (argc == 1) {
        config_.command = "";  // Empty command signals error
        return;
    }
    
    // If we get here, we have flags but no command - also an error
    config_.command = "";
}

void TrayApp::print_usage(bool show_serve_options) {
    std::cout << "lemonade-server - Lemonade Server\n\n";
    std::cout << "Usage: lemonade-server <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  serve                    Start the server\n";
    std::cout << "  run <model>              Run a model\n";
    std::cout << "  list                     List available models\n";
    std::cout << "  pull <model>             Download a model\n";
    std::cout << "  delete <model>           Delete a model\n";
    std::cout << "  status                   Check server status\n";
    std::cout << "  stop                     Stop the server\n\n";
    
    // Only show serve options if requested (for serve/run --help)
    if (show_serve_options) {
        std::cout << "Serve/Run Options:\n";
        std::cout << "  --port PORT              Server port (default: 8000)\n";
        std::cout << "  --host HOST              Server host (default: 127.0.0.1)\n";
        std::cout << "  --ctx-size SIZE          Context size (default: 4096)\n";
        std::cout << "  --llamacpp BACKEND       LlamaCpp backend: vulkan, rocm, metal, cpu (default: vulkan)\n";
        std::cout << "  --llamacpp-args ARGS     Custom arguments for llama-server\n";
        std::cout << "  --max-loaded-models N [E] [R]\n";
        std::cout << "                           Max loaded models: LLMS [EMBEDDINGS] [RERANKINGS] (default: 1 1 1)\n";
        std::cout << "  --log-file PATH          Log file path\n";
        std::cout << "  --log-level LEVEL        Log level: info, debug, trace (default: info)\n";
#if defined(__linux__) && !defined(__ANDROID__)
        std::cout << "  --no-tray                Start server without tray (default on Linux)\n";
#else
        std::cout << "  --no-tray                Start server without tray (headless mode)\n";
#endif
        std::cout << "\n";
    }
    
    std::cout << "  --help, -h               Show this help message\n";
    std::cout << "  --version, -v            Show version\n";
}

void TrayApp::print_version() {
    std::cout << "lemonade-server version " << current_version_ << std::endl;
}

void TrayApp::print_pull_help() {
    std::cout << "lemonade-server pull - Download and install a model\n\n";
    std::cout << "Usage:\n";
    std::cout << "  lemonade-server pull <model_name> [options]\n\n";
    std::cout << "Description:\n";
    std::cout << "  Downloads a model from the Lemonade Server registry or Hugging Face.\n";
    std::cout << "  For registered models, only the model name is required.\n";
    std::cout << "  For custom models, use the registration options below.\n\n";
    std::cout << "Registration Options (for custom models):\n";
    std::cout << "  --checkpoint CHECKPOINT  Hugging Face checkpoint (format: org/model:variant)\n";
    std::cout << "  --recipe RECIPE          Inference recipe to use\n";
    std::cout << "                           Options: llamacpp, flm, oga-cpu, oga-hybrid, oga-npu\n\n";
    std::cout << "  --reasoning              Mark model as a reasoning model (e.g., DeepSeek-R1)\n";
    std::cout << "                           Adds 'reasoning' label to model metadata.\n\n";
    std::cout << "  --vision                 Mark model as a vision model (multimodal)\n";
    std::cout << "                           Adds 'vision' label to model metadata.\n\n";
    std::cout << "  --embedding              Mark model as an embedding model\n";
    std::cout << "                           Adds 'embeddings' label to model metadata.\n";
    std::cout << "                           For use with /api/v1/embeddings endpoint.\n\n";
    std::cout << "  --reranking              Mark model as a reranking model\n";
    std::cout << "                           Adds 'reranking' label to model metadata.\n";
    std::cout << "                           For use with /api/v1/reranking endpoint.\n\n";
    std::cout << "  --mmproj FILENAME        Multimodal projector file for vision models\n";
    std::cout << "                           Required for GGUF vision models.\n";
    std::cout << "                           Example: mmproj-model-f16.gguf\n\n";
}

bool TrayApp::find_server_binary() {
    // Look for lemonade binary in common locations
    std::vector<std::string> search_paths;
    
#ifdef _WIN32
    std::string binary_name = "lemonade-router.exe";
    
    // Get the directory where this executable is located
    char exe_path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path_buf, MAX_PATH);
    if (len > 0) {
        fs::path exe_dir = fs::path(exe_path_buf).parent_path();
        // First priority: same directory as this executable
        search_paths.push_back((exe_dir / binary_name).string());
    }
#else
    std::string binary_name = "lemonade-router";
    
    // On Unix, try to get executable path
    char exe_path_buf[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len != -1) {
        exe_path_buf[len] = '\0';
        fs::path exe_dir = fs::path(exe_path_buf).parent_path();
        search_paths.push_back((exe_dir / binary_name).string());
    }
#endif
    
    // Current directory
    search_paths.push_back(binary_name);
    
    // Parent directory
    search_paths.push_back("../" + binary_name);
    
    // Common install locations
#ifdef _WIN32
    search_paths.push_back("C:/Program Files/Lemonade/" + binary_name);
#else
    search_paths.push_back("/usr/local/bin/" + binary_name);
    search_paths.push_back("/usr/bin/" + binary_name);
#endif
    
    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            config_.server_binary = fs::absolute(path).string();
            DEBUG_LOG(this, "Found server binary: " << config_.server_binary);
            return true;
        }
    }
    
    return false;
}

bool TrayApp::setup_logging() {
    // TODO: Implement logging setup
    return true;
}

// Helper: Check if server is running on a specific port
bool TrayApp::is_server_running_on_port(int port) {
    try {
        auto health = server_manager_->get_health();
        return true;
    } catch (...) {
        return false;
    }
}

// Helper: Get server info (returns {pid, port} or {0, 0} if not found)
std::pair<int, int> TrayApp::get_server_info() {
    // Query OS for listening TCP connections and find lemonade-router.exe
#ifdef _WIN32
    // Windows: Use GetExtendedTcpTable to find listening connections
    // Check both IPv4 and IPv6 since server may bind to either
    
    // Helper lambda to check if a PID is lemonade-router.exe
    auto is_lemonade_router = [](DWORD pid) -> bool {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProcess) {
            WCHAR processName[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, processName, &size)) {
                std::wstring fullPath(processName);
                std::wstring exeName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
                CloseHandle(hProcess);
                return (exeName == L"lemonade-router.exe");
            }
            CloseHandle(hProcess);
        }
        return false;
    };
    
    // Try IPv4 first
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    
    std::vector<BYTE> buffer(size);
    PMIB_TCPTABLE_OWNER_PID pTcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    
    if (GetExtendedTcpTable(pTcpTable, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
            DWORD pid = pTcpTable->table[i].dwOwningPid;
            int port = ntohs((u_short)pTcpTable->table[i].dwLocalPort);
            
            if (is_lemonade_router(pid)) {
                return {static_cast<int>(pid), port};
            }
        }
    }
    
    // Try IPv6 if not found in IPv4
    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
    
    buffer.resize(size);
    PMIB_TCP6TABLE_OWNER_PID pTcp6Table = reinterpret_cast<PMIB_TCP6TABLE_OWNER_PID>(buffer.data());
    
    if (GetExtendedTcpTable(pTcp6Table, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        for (DWORD i = 0; i < pTcp6Table->dwNumEntries; i++) {
            DWORD pid = pTcp6Table->table[i].dwOwningPid;
            int port = ntohs((u_short)pTcp6Table->table[i].dwLocalPort);
            
            if (is_lemonade_router(pid)) {
                return {static_cast<int>(pid), port};
            }
        }
    }
#else
    // Unix: Read from PID file
    std::ifstream pid_file("/tmp/lemonade-router.pid");
    if (pid_file.is_open()) {
        int pid, port;
        pid_file >> pid >> port;
        pid_file.close();
        
        // Verify the PID is still alive
        if (kill(pid, 0) == 0) {
            return {pid, port};
        }
        
        // Stale PID file, remove it
        remove("/tmp/lemonade-router.pid");
    }
#endif
    
    return {0, 0};  // Server not found
}

// Helper: Start ephemeral server
bool TrayApp::start_ephemeral_server(int port) {
    if (!server_manager_) {
        server_manager_ = std::make_unique<ServerManager>();
    }
    
    DEBUG_LOG(this, "Starting ephemeral server on port " << port << "...");
    
    bool success = server_manager_->start_server(
        config_.server_binary,
        port,
        config_.ctx_size,
        config_.log_file.empty() ? "" : config_.log_file,
        config_.log_level,  // Pass log level to ServerManager
        config_.llamacpp_backend,  // Pass llamacpp backend to ServerManager
        false,  // show_console - SSE streaming provides progress via client
        true,   // is_ephemeral (suppress startup message)
        config_.llamacpp_args,  // Pass custom llamacpp args
        config_.host,  // Pass host to ServerManager
        config_.max_llm_models,
        config_.max_embedding_models,
        config_.max_reranking_models
    );
    
    if (!success) {
        std::cerr << "Failed to start ephemeral server" << std::endl;
        return false;
    }
    
    return true;
}

// Command: list
int TrayApp::execute_list_command() {
    DEBUG_LOG(this, "Listing available models...");
    
    // Check if server is running
    auto [pid, running_port] = get_server_info();
    bool server_was_running = (running_port != 0);
    int port = server_was_running ? running_port : config_.port;
    
    // Start ephemeral server if needed
    if (!server_was_running) {
        if (!start_ephemeral_server(port)) {
            return 1;
        }
    }
    
    // Get models from server with show_all=true to include download status
    try {
        if (!server_manager_) {
            server_manager_ = std::make_unique<ServerManager>();
        }
        server_manager_->set_port(port);  // Use the detected or configured port
        
        // Request with show_all=true to get download status
        std::string response = server_manager_->make_http_request("/api/v1/models?show_all=true");
        auto models_json = nlohmann::json::parse(response);
        
        if (!models_json.contains("data") || !models_json["data"].is_array()) {
            std::cerr << "Invalid response format from server" << std::endl;
            if (!server_was_running) stop_server();
            return 1;
        }
        
        // Print models in a nice table format
        std::cout << std::left << std::setw(40) << "Model Name"
                  << std::setw(12) << "Downloaded"
                  << "Details" << std::endl;
        std::cout << std::string(100, '-') << std::endl;
        
        for (const auto& model : models_json["data"]) {
            std::string name = model.value("id", "unknown");
            bool is_downloaded = model.value("downloaded", false);
            std::string downloaded = is_downloaded ? "Yes" : "No";
            std::string details = model.value("recipe", "-");
            
            std::cout << std::left << std::setw(40) << name
                      << std::setw(12) << downloaded
                      << details << std::endl;
        }
        
        std::cout << std::string(100, '-') << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error listing models: " << e.what() << std::endl;
        if (!server_was_running) stop_server();
        return 1;
    }
    
    // Stop ephemeral server
    if (!server_was_running) {
        DEBUG_LOG(this, "Stopping ephemeral server...");
        stop_server();
    }
    
    return 0;
}

// Command: pull
int TrayApp::execute_pull_command() {
    if (config_.command_args.empty()) {
        std::cerr << "Error: model name required" << std::endl;
        std::cerr << "Usage: lemonade-server pull <model_name> [--checkpoint CHECKPOINT] [--recipe RECIPE] [--reasoning] [--vision] [--embedding] [--reranking] [--mmproj MMPROJ]" << std::endl;
        return 1;
    }
    
    std::string model_name = config_.command_args[0];
    std::cout << "Pulling model: " << model_name << std::endl;
    
    // Check if server is running
    auto [pid, running_port] = get_server_info();
    bool server_was_running = (running_port != 0);
    int port = server_was_running ? running_port : config_.port;
    
    // Start ephemeral server if needed
    if (!server_was_running) {
        if (!start_ephemeral_server(port)) {
            return 1;
        }
    }
    
    // Pull model via API with SSE streaming for progress
    try {
        // Build request body with all optional parameters
        nlohmann::json request_body = {{"model", model_name}, {"stream", true}};
        
        // Parse optional arguments from config_.command_args (starting at index 1)
        for (size_t i = 1; i < config_.command_args.size(); ++i) {
            const auto& arg = config_.command_args[i];
            
            if (arg == "--checkpoint" && i + 1 < config_.command_args.size()) {
                request_body["checkpoint"] = config_.command_args[++i];
            } else if (arg == "--recipe" && i + 1 < config_.command_args.size()) {
                request_body["recipe"] = config_.command_args[++i];
            } else if (arg == "--reasoning") {
                request_body["reasoning"] = true;
            } else if (arg == "--vision") {
                request_body["vision"] = true;
            } else if (arg == "--embedding") {
                request_body["embedding"] = true;
            } else if (arg == "--reranking") {
                request_body["reranking"] = true;
            } else if (arg == "--mmproj" && i + 1 < config_.command_args.size()) {
                request_body["mmproj"] = config_.command_args[++i];
            }
        }
        
        // Use SSE streaming to receive progress events
        // Use the same host the server is bound to (0.0.0.0 is special - use localhost instead)
        std::string connect_host = (config_.host == "0.0.0.0") ? "localhost" : config_.host;
        
        httplib::Client cli(connect_host, port);
        cli.set_connection_timeout(30, 0);
        cli.set_read_timeout(86400, 0);  // 24 hour read timeout for large downloads
        
        std::string last_file;
        int last_percent = -1;
        bool success = false;
        std::string error_message;
        std::string buffer;  // Buffer for partial SSE messages
        
        httplib::Headers headers;
        auto res = cli.Post("/api/v1/pull", headers, request_body.dump(), "application/json",
            [&](const char* data, size_t len) {
                buffer.append(data, len);
                
                // Process complete SSE messages (end with \n\n)
                size_t pos;
                while ((pos = buffer.find("\n\n")) != std::string::npos) {
                    std::string message = buffer.substr(0, pos);
                    buffer.erase(0, pos + 2);
                    
                    // Parse SSE event
                    std::string event_type;
                    std::string event_data;
                    
                    std::istringstream stream(message);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (line.substr(0, 6) == "event:") {
                            event_type = line.substr(7);
                            // Trim whitespace
                            while (!event_type.empty() && event_type[0] == ' ') {
                                event_type.erase(0, 1);
                            }
                        } else if (line.substr(0, 5) == "data:") {
                            event_data = line.substr(6);
                            // Trim whitespace
                            while (!event_data.empty() && event_data[0] == ' ') {
                                event_data.erase(0, 1);
                            }
                        }
                    }
                    
                    if (!event_data.empty()) {
                        try {
                            auto json_data = nlohmann::json::parse(event_data);
                            
                            if (event_type == "progress") {
                                std::string file = json_data.value("file", "");
                                int file_index = json_data.value("file_index", 0);
                                int total_files = json_data.value("total_files", 0);
                                // Use uint64_t explicitly to avoid JSON type inference issues with large numbers
                                uint64_t bytes_downloaded = json_data.value("bytes_downloaded", (uint64_t)0);
                                uint64_t bytes_total = json_data.value("bytes_total", (uint64_t)0);
                                int percent = json_data.value("percent", 0);
                                
                                // Only print when file changes or percent changes significantly
                                if (file != last_file) {
                                    if (!last_file.empty()) {
                                        std::cout << std::endl;  // New line after previous file
                                    }
                                    std::cout << "[" << file_index << "/" << total_files << "] " << file;
                                    if (bytes_total > 0) {
                                        std::cout << " (" << std::fixed << std::setprecision(1) 
                                                  << (bytes_total / (1024.0 * 1024.0)) << " MB)";
                                    }
                                    std::cout << std::endl;
                                    last_file = file;
                                    last_percent = -1;
                                }
                                
                                // Update progress bar
                                if (bytes_total > 0 && percent != last_percent) {
                                    std::cout << "\r  Progress: " << percent << "% (" 
                                              << std::fixed << std::setprecision(1)
                                              << (bytes_downloaded / (1024.0 * 1024.0)) << "/"
                                              << (bytes_total / (1024.0 * 1024.0)) << " MB)" << std::flush;
                                    last_percent = percent;
                                }
                            } else if (event_type == "complete") {
                                std::cout << std::endl;
                                success = true;
                            } else if (event_type == "error") {
                                error_message = json_data.value("error", "Unknown error");
                            }
                        } catch (const std::exception&) {
                            // Ignore JSON parse errors in SSE events
                        }
                    }
                }
                
                return true;  // Continue receiving
            });
        
        // Check for errors - but ignore connection close if we got a success event
        if (!res && !success) {
            throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
        }
        
        if (!error_message.empty()) {
            throw std::runtime_error(error_message);
        }
        
        if (success) {
            std::cout << "Model pulled successfully: " << model_name << std::endl;
        } else if (!res) {
            // Connection closed without success - this is an error
            throw std::runtime_error("Connection closed unexpectedly");
        } else {
            std::cerr << "Pull completed without success confirmation" << std::endl;
            if (!server_was_running) stop_server();
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error pulling model: " << e.what() << std::endl;
        if (!server_was_running) stop_server();
        return 1;
    }
    
    // Stop ephemeral server
    if (!server_was_running) {
        DEBUG_LOG(this, "Stopping ephemeral server...");
        stop_server();
    }
    
    return 0;
}

// Command: delete
int TrayApp::execute_delete_command() {
    if (config_.command_args.empty()) {
        std::cerr << "Error: model name required" << std::endl;
        std::cerr << "Usage: lemonade-server delete <model_name>" << std::endl;
        return 1;
    }
    
    std::string model_name = config_.command_args[0];
    std::cout << "Deleting model: " << model_name << std::endl;
    
    // Check if server is running
    auto [pid, running_port] = get_server_info();
    bool server_was_running = (running_port != 0);
    int port = server_was_running ? running_port : config_.port;
    
    // Start ephemeral server if needed
    if (!server_was_running) {
        if (!start_ephemeral_server(port)) {
            return 1;
        }
    }
    
    // Delete model via API
    try {
        if (!server_manager_) {
            server_manager_ = std::make_unique<ServerManager>();
        }
        server_manager_->set_port(port);  // Use the detected or configured port
        
        nlohmann::json request_body = {{"model", model_name}};
        std::string response = server_manager_->make_http_request(
            "/api/v1/delete", 
            "POST", 
            request_body.dump()
        );
        
        auto response_json = nlohmann::json::parse(response);
        if (response_json.value("status", "") == "success") {
            std::cout << "Model deleted successfully: " << model_name << std::endl;
        } else {
            std::cerr << "Failed to delete model" << std::endl;
            if (!server_was_running) stop_server();
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error deleting model: " << e.what() << std::endl;
        if (!server_was_running) stop_server();
        return 1;
    }
    
    // Stop ephemeral server
    if (!server_was_running) {
        DEBUG_LOG(this, "Stopping ephemeral server...");
        stop_server();
    }
    
    return 0;
}

// Command: run
int TrayApp::execute_run_command() {
    if (config_.command_args.empty()) {
        std::cerr << "Error: model name required" << std::endl;
        std::cerr << "Usage: lemonade-server run <model_name>" << std::endl;
        return 1;
    }
    
    std::string model_name = config_.command_args[0];
    std::cout << "Running model: " << model_name << std::endl;
    
    // The run command will:
    // 1. Start server (already done in main run() before this function is called)
    // 2. Load the model
    // 3. Open browser
    // 4. Show tray (handled by main run() after this returns)
    
    // Note: Server is already started and ready - start_server() does health checks internally
    
    // Load the model
    std::cout << "Loading model " << model_name << "..." << std::endl;
    if (server_manager_->load_model(model_name)) {
        std::cout << "Model loaded successfully!" << std::endl;
        
        // Open browser to chat interface
        std::string url = "http://" + config_.host + ":" + std::to_string(config_.port) + "/?model=" + model_name + "#llm-chat";
        std::cout << "Opening browser: " << url << std::endl;
        open_url(url);
    } else {
        std::cerr << "Failed to load model" << std::endl;
        return 1;
    }
    
    // Return success - main run() will continue to tray initialization or wait loop
    return 0;
}

// Command: status
int TrayApp::execute_status_command() {
    auto [pid, port] = get_server_info();
    
    if (port != 0) {
        std::cout << "Server is running on port " << port << std::endl;
        return 0;
    } else {
        std::cout << "Server is not running" << std::endl;
        return 1;
    }
}

// Command: stop
int TrayApp::execute_stop_command() {
    auto [pid, port] = get_server_info();
    
    if (port == 0) {
        std::cout << "Lemonade Server is not running" << std::endl;
        return 0;
    }
    
    std::cout << "Stopping server on port " << port << "..." << std::endl;
    
    // Match Python's stop() behavior exactly:
    // 1. Get main process and children
    // 2. Send terminate (SIGTERM) to main and llama-server children
    // 3. Wait 5 seconds
    // 4. If timeout, send kill (SIGKILL) to main and children
    
#ifdef _WIN32
    // Use the PID we already got from get_server_info() (the process listening on the port)
    // This is the router process
    DWORD router_pid = static_cast<DWORD>(pid);
    std::cout << "Found router process (PID: " << router_pid << ")" << std::endl;
    
    // Find the parent tray app (if it exists)
    DWORD tray_pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == router_pid) {
                    // Found router, check its parent
                    DWORD parent_pid = pe32.th32ParentProcessID;
                    // Search for parent to see if it's lemonade-server
                    if (Process32FirstW(snapshot, &pe32)) {
                        do {
                            if (pe32.th32ProcessID == parent_pid) {
                                std::wstring parent_name(pe32.szExeFile);
                                if (parent_name == L"lemonade-server.exe") {
                                    tray_pid = parent_pid;
                                    std::cout << "Found parent tray app (PID: " << tray_pid << ")" << std::endl;
                                }
                                break;
                            }
                        } while (Process32NextW(snapshot, &pe32));
                    }
                    break;
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    // Windows limitation: TerminateProcess doesn't trigger signal handlers (it's like SIGKILL)
    // So we must explicitly kill children since router won't get a chance to clean up
    // First, collect all children
    std::vector<DWORD> child_pids;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (pe32.th32ParentProcessID == router_pid) {
                    child_pids.push_back(pe32.th32ProcessID);
                    std::wstring process_name(pe32.szExeFile);
                    std::wcout << L"  Found child process: " << process_name 
                               << L" (PID: " << pe32.th32ProcessID << L")" << std::endl;
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    // Terminate router process
    std::cout << "Terminating router (PID: " << router_pid << ")..." << std::endl;
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, router_pid);
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
    }
    
    // Terminate children (Windows can't do graceful shutdown from outside)
    for (DWORD child_pid : child_pids) {
        std::cout << "Terminating child process (PID: " << child_pid << ")..." << std::endl;
        HANDLE hChild = OpenProcess(PROCESS_TERMINATE, FALSE, child_pid);
        if (hChild) {
            TerminateProcess(hChild, 0);
            CloseHandle(hChild);
        }
    }
    
    // Terminate tray app parent if it exists
    if (tray_pid != 0) {
        std::cout << "Terminating tray app (PID: " << tray_pid << ")..." << std::endl;
        HANDLE hTray = OpenProcess(PROCESS_TERMINATE, FALSE, tray_pid);
        if (hTray) {
            TerminateProcess(hTray, 0);
            CloseHandle(hTray);
        }
    }
    
    // Wait up to 5 seconds for processes to exit
    std::cout << "Waiting for processes to exit (up to 5 seconds)..." << std::endl;
    bool exited_gracefully = false;
    for (int i = 0; i < 50; i++) {  // 50 * 100ms = 5 seconds
        bool found_router = false;
        bool found_tray = false;
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe32;
            pe32.dwSize = sizeof(pe32);
            
            if (Process32FirstW(snapshot, &pe32)) {
                do {
                    if (pe32.th32ProcessID == router_pid) {
                        found_router = true;
                    }
                    if (tray_pid != 0 && pe32.th32ProcessID == tray_pid) {
                        found_tray = true;
                    }
                } while (Process32NextW(snapshot, &pe32));
            }
            CloseHandle(snapshot);
        }
        
        // Both router and tray (if it existed) must be gone
        if (!found_router && (tray_pid == 0 || !found_tray)) {
            exited_gracefully = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (exited_gracefully) {
        std::cout << "Lemonade Server stopped successfully." << std::endl;
        return 0;
    }
    
    // Timeout expired, force kill
    std::cout << "Timeout expired, forcing termination..." << std::endl;
    
    // Force kill router process
    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, router_pid);
    if (hProcess) {
        std::cout << "Force killing router (PID: " << router_pid << ")" << std::endl;
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
    }
    
    // Force kill tray app if it exists
    if (tray_pid != 0) {
        HANDLE hTray = OpenProcess(PROCESS_TERMINATE, FALSE, tray_pid);
        if (hTray) {
            std::cout << "Force killing tray app (PID: " << tray_pid << ")" << std::endl;
            TerminateProcess(hTray, 0);
            CloseHandle(hTray);
        }
    }
    
    // Force kill any remaining orphaned processes (shouldn't be any at this point)
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == router_pid || 
                    (tray_pid != 0 && pe32.th32ProcessID == tray_pid) ||
                    pe32.th32ParentProcessID == router_pid) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProc) {
                        std::wstring process_name(pe32.szExeFile);
                        std::wcout << L"Force killing remaining process: " << process_name 
                                   << L" (PID: " << pe32.th32ProcessID << L")" << std::endl;
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    // Note: log-viewer.exe auto-exits when parent process dies, no need to explicitly kill it
#else
    // Unix: Use the PID we already got from get_server_info() (this is the router)
    int router_pid = pid;
    std::cout << "Found router process (PID: " << router_pid << ")" << std::endl;
    
    // Find parent tray app if it exists
    int tray_pid = 0;
    std::string ppid_cmd = "ps -o ppid= -p " + std::to_string(router_pid);
    FILE* pipe = popen(ppid_cmd.c_str(), "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            int parent_pid = atoi(buffer);
            // Check if parent is lemonade-server
            std::string name_cmd = "ps -o comm= -p " + std::to_string(parent_pid);
            FILE* name_pipe = popen(name_cmd.c_str(), "r");
            if (name_pipe) {
                char name_buf[128];
                if (fgets(name_buf, sizeof(name_buf), name_pipe) != nullptr) {
                    std::string parent_name(name_buf);
                    // Remove newline
                    parent_name.erase(parent_name.find_last_not_of("\n\r") + 1);
                    // Note: ps -o comm= is limited to 15 chars on Linux (/proc/PID/comm truncation)
                    // "lemonade-server" is exactly 15 chars, so no truncation occurs
                    if (parent_name.find("lemonade-server") != std::string::npos) {
                        tray_pid = parent_pid;
                        std::cout << "Found parent tray app (PID: " << tray_pid << ")" << std::endl;
                    }
                }
                pclose(name_pipe);
            }
        }
        pclose(pipe);
    }
    
    // Find router's children BEFORE killing anything (they get reparented after router exits)
    std::vector<int> router_children;
    pipe = popen(("pgrep -P " + std::to_string(router_pid)).c_str(), "r");
    if (pipe) {
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            int child_pid = atoi(buffer);
            if (child_pid > 0) {
                router_children.push_back(child_pid);
            }
        }
        pclose(pipe);
    }
    
    if (!router_children.empty()) {
        std::cout << "Found " << router_children.size() << " child process(es) of router" << std::endl;
    }
    
    // Send SIGTERM to router (it will exit via _exit() immediately)
    std::cout << "Sending SIGTERM to router (PID: " << router_pid << ")..." << std::endl;
    kill(router_pid, SIGTERM);
    
    // Also send SIGTERM to parent tray app if it exists
    if (tray_pid != 0) {
        std::cout << "Sending SIGTERM to tray app (PID: " << tray_pid << ")..." << std::endl;
        kill(tray_pid, SIGTERM);
    }
    
    // Send SIGTERM to child processes immediately (matching Python's stop() behavior)
    // Since router exits via _exit(), it won't clean up children itself
    if (!router_children.empty()) {
        std::cout << "Sending SIGTERM to child processes..." << std::endl;
        for (int child_pid : router_children) {
            if (kill(child_pid, 0) == 0) {  // Check if still alive
                kill(child_pid, SIGTERM);
            }
        }
    }
    
    // Wait up to 5 seconds for processes to exit gracefully
    // This matches Python's stop() behavior: terminate everything, then wait
    std::cout << "Waiting for processes to exit (up to 5 seconds)..." << std::endl;
    bool exited_gracefully = false;
    
    for (int i = 0; i < 50; i++) {  // 50 * 100ms = 5 seconds
        // Check if main processes are completely gone from process table
        bool router_gone = !std::filesystem::exists("/proc/" + std::to_string(router_pid));
        bool tray_gone = (tray_pid == 0 || !std::filesystem::exists("/proc/" + std::to_string(tray_pid)));
        
        // Check if all children have exited
        bool all_children_gone = true;
        for (int child_pid : router_children) {
            if (std::filesystem::exists("/proc/" + std::to_string(child_pid))) {
                all_children_gone = false;
                break;
            }
        }
        
        // Both main processes and all children must be gone
        if (router_gone && tray_gone && all_children_gone) {
            // Additional check: verify the lock file can be acquired
            // This is a belt-and-suspenders check to ensure the lock is truly released
            std::string lock_file = "/tmp/lemonade_Server.lock";
            int fd = open(lock_file.c_str(), O_RDWR | O_CREAT, 0666);
            if (fd != -1) {
                if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                    std::cout << "All processes exited, shutdown complete!" << std::endl;
                    flock(fd, LOCK_UN);
                    close(fd);
                    exited_gracefully = true;
                    break;
                } else {
                    // Lock still held somehow - wait a bit more
                    close(fd);
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!exited_gracefully) {
        // Timeout expired, force kill everything that's still alive
        // This matches Python's stop() behavior
        std::cout << "Timeout expired, forcing termination..." << std::endl;
        
        // Force kill router process (if still alive)
        if (std::filesystem::exists("/proc/" + std::to_string(router_pid))) {
            std::cout << "Force killing router (PID: " << router_pid << ")" << std::endl;
            kill(router_pid, SIGKILL);
        }
        
        // Force kill tray app if it exists
        if (tray_pid != 0 && std::filesystem::exists("/proc/" + std::to_string(tray_pid))) {
            std::cout << "Force killing tray app (PID: " << tray_pid << ")" << std::endl;
            kill(tray_pid, SIGKILL);
        }
        
        // Force kill any remaining children (matching Python's behavior for stubborn llama-server)
        if (!router_children.empty()) {
            for (int child_pid : router_children) {
                if (std::filesystem::exists("/proc/" + std::to_string(child_pid))) {
                    std::cout << "Force killing child process (PID: " << child_pid << ")" << std::endl;
                    kill(child_pid, SIGKILL);
                }
            }
        }
    }
#endif
    
    std::cout << "Lemonade Server stopped successfully." << std::endl;
    return 0;
}

bool TrayApp::start_server() {
    // Set default log file if not specified
    if (config_.log_file.empty()) {
        #ifdef _WIN32
        // Windows: %TEMP%\lemonade-server.log
        char* temp_path = nullptr;
        size_t len = 0;
        _dupenv_s(&temp_path, &len, "TEMP");
        if (temp_path) {
            config_.log_file = std::string(temp_path) + "\\lemonade-server.log";
            free(temp_path);
        } else {
            config_.log_file = "lemonade-server.log";
        }
        #else
        // Unix: /tmp/lemonade-server.log or ~/.lemonade/server.log
        config_.log_file = "/tmp/lemonade-server.log";
        #endif
        DEBUG_LOG(this, "Using default log file: " << config_.log_file);
    }
    
    bool success = server_manager_->start_server(
        config_.server_binary,
        config_.port,
        config_.ctx_size,
        config_.log_file,
        config_.log_level,  // Pass log level to ServerManager
        config_.llamacpp_backend,  // Pass llamacpp backend to ServerManager
        true,               // Always show console output for serve command
        false,              // is_ephemeral = false (persistent server, show startup message with URL)
        config_.llamacpp_args,  // Pass custom llamacpp args
        config_.host,        // Pass host to ServerManager
        config_.max_llm_models,
        config_.max_embedding_models,
        config_.max_reranking_models
    );
    
    // Start log tail thread to show logs in console
    if (success) {
        stop_tail_thread_ = false;
        log_tail_thread_ = std::thread(&TrayApp::tail_log_to_console, this);
    }
    
    return success;
}

void TrayApp::stop_server() {
    // Stop log tail thread
    if (log_tail_thread_.joinable()) {
        stop_tail_thread_ = true;
        log_tail_thread_.join();
    }
    
    if (server_manager_) {
        server_manager_->stop_server();
    }
}

void TrayApp::build_menu() {
    if (!tray_) return;
    
    Menu menu = create_menu();
    tray_->set_menu(menu);
}

Menu TrayApp::create_menu() {
    Menu menu;
    
    // Get all loaded models to display at top and for checkmarks
    std::vector<LoadedModelInfo> loaded_models = is_loading_model_ ? std::vector<LoadedModelInfo>() : get_all_loaded_models();
    
    // Build a set of loaded model names for quick lookup
    std::set<std::string> loaded_model_names;
    for (const auto& m : loaded_models) {
        loaded_model_names.insert(m.model_name);
    }
    
    // Status display - show all loaded models at the top
    if (is_loading_model_) {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(loading_mutex_));
        menu.add_item(MenuItem::Action("Loading: " + loading_model_name_ + "...", nullptr, false));
    } else {
        if (!loaded_models.empty()) {
            // Show each loaded model with its type
            for (const auto& model : loaded_models) {
                std::string display_text = "Loaded: " + model.model_name;
                if (!model.type.empty() && model.type != "llm") {
                    display_text += " (" + model.type + ")";
                }
                menu.add_item(MenuItem::Action(display_text, nullptr, false));
            }
        } else {
            menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
        }
    }
    
    // Unload Model submenu
    auto unload_submenu = std::make_shared<Menu>();
    if (loaded_models.empty()) {
        unload_submenu->add_item(MenuItem::Action(
            "No models loaded",
            nullptr,
            false
        ));
    } else {
        for (const auto& model : loaded_models) {
            // Display model name with type if not LLM
            std::string display_text = model.model_name;
            if (!model.type.empty() && model.type != "llm") {
                display_text += " (" + model.type + ")";
            }
            unload_submenu->add_item(MenuItem::Action(
                display_text,
                [this, model_name = model.model_name]() { on_unload_specific_model(model_name); }
            ));
        }
        
        // Add "Unload all" option if multiple models are loaded
        if (loaded_models.size() > 1) {
            unload_submenu->add_separator();
            unload_submenu->add_item(MenuItem::Action(
                "Unload all",
                [this]() { on_unload_model(); }
            ));
        }
    }
    menu.add_item(MenuItem::Submenu("Unload Model", unload_submenu));
    
    // Load Model submenu
    auto load_submenu = std::make_shared<Menu>();
    auto models = get_downloaded_models();
    if (models.empty()) {
        load_submenu->add_item(MenuItem::Action(
            "No models available: Use the Model Manager",
            nullptr,
            false
        ));
    } else {
        for (const auto& model : models) {
            // Check if this model is in the loaded models set
            bool is_loaded = loaded_model_names.count(model.id) > 0;
            load_submenu->add_item(MenuItem::Checkable(
                model.id,
                [this, model]() { on_load_model(model.id); },
                is_loaded
            ));
        }
    }
    menu.add_item(MenuItem::Submenu("Load Model", load_submenu));
    
    // Port submenu
    auto port_submenu = std::make_shared<Menu>();
    std::vector<int> ports = {8000, 8020, 8040, 8060, 8080, 9000};
    for (int port : ports) {
        bool is_current = (port == config_.port);
        port_submenu->add_item(MenuItem::Checkable(
            "Port " + std::to_string(port),
            [this, port]() { on_change_port(port); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Port", port_submenu));
    
    // Context Size submenu
    auto ctx_submenu = std::make_shared<Menu>();
    std::vector<std::pair<std::string, int>> ctx_sizes = {
        {"4K", 4096}, {"8K", 8192}, {"16K", 16384},
        {"32K", 32768}, {"64K", 65536}, {"128K", 131072}
    };
    for (const auto& [label, size] : ctx_sizes) {
        bool is_current = (size == config_.ctx_size);
        ctx_submenu->add_item(MenuItem::Checkable(
            "Context size " + label,
            [this, size]() { on_change_context_size(size); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Context Size", ctx_submenu));
    
    menu.add_separator();
    
    // Main menu items
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));
    menu.add_item(MenuItem::Action("LLM Chat", [this]() { on_open_llm_chat(); }));
    menu.add_item(MenuItem::Action("Model Manager", [this]() { on_open_model_manager(); }));
    
    // Logs menu item (simplified - always debug logs now)
    menu.add_item(MenuItem::Action("Show Logs", [this]() { on_show_logs(); }));
    
    menu.add_separator();
    menu.add_item(MenuItem::Action("Quit Lemonade", [this]() { on_quit(); }));
    
    return menu;
}

// Menu action implementations

void TrayApp::on_load_model(const std::string& model_name) {
    // CRITICAL: Make a copy IMMEDIATELY since model_name is a reference that gets invalidated
    // when build_menu() destroys the old menu (which destroys the lambda that captured the model)
    std::string model_name_copy = model_name;
    
    // Don't start a new load if one is already in progress
    if (is_loading_model_) {
        show_notification("Model Loading", "A model is already being loaded. Please wait.");
        return;
    }
    
    std::cout << "Loading model: '" << model_name_copy << "' (length: " << model_name_copy.length() << ")" << std::endl;
    std::cout.flush();
    
    // Set loading state
    {
        std::lock_guard<std::mutex> lock(loading_mutex_);
        is_loading_model_ = true;
        loading_model_name_ = model_name_copy;
    }
    
    // Update menu to show loading status
    build_menu();
    
    // Launch background thread to perform the load
    std::thread([this, model_name_copy]() {
        std::cout << "Background thread: Loading model: '" << model_name_copy << "' (length: " << model_name_copy.length() << ")" << std::endl;
        std::cout.flush();
        
        bool success = server_manager_->load_model(model_name_copy);
        
        // Update state after load completes
        {
            std::lock_guard<std::mutex> lock(loading_mutex_);
            is_loading_model_ = false;
            if (success) {
                loaded_model_ = model_name_copy;
            }
        }
        
        // Update menu to show new status
        build_menu();
        
        if (success) {
            show_notification("Model Loaded", "Successfully loaded " + model_name_copy);
        } else {
            show_notification("Load Failed", "Failed to load " + model_name_copy);
        }
    }).detach();
}

void TrayApp::on_unload_model() {
    // Don't allow unload while a model is loading
    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }
    
    std::cout << "Unloading all models" << std::endl;
    if (server_manager_->unload_model()) {
        loaded_model_.clear();
        build_menu();
    }
}

void TrayApp::on_unload_specific_model(const std::string& model_name) {
    // Copy to avoid reference invalidation when menu is rebuilt
    std::string model_name_copy = model_name;
    
    // Don't allow unload while a model is loading
    if (is_loading_model_) {
        show_notification("Model Loading", "Please wait for the current model to finish loading.");
        return;
    }
    
    std::cout << "Unloading model: '" << model_name_copy << "'" << std::endl;
    std::cout.flush();
    
    // Launch background thread to perform the unload
    std::thread([this, model_name_copy]() {
        std::cout << "Background thread: Unloading model: '" << model_name_copy << "'" << std::endl;
        std::cout.flush();
        
        server_manager_->unload_model(model_name_copy);
        
        // Update menu to show new status
        build_menu();
    }).detach();
}

void TrayApp::on_change_port(int new_port) {
    std::cout << "Changing port to: " << new_port << std::endl;
    config_.port = new_port;
    server_manager_->set_port(new_port);
    build_menu();
    show_notification("Port Changed", "Lemonade Server is now running on port " + std::to_string(new_port));
}

void TrayApp::on_change_context_size(int new_ctx_size) {
    std::cout << "Changing context size to: " << new_ctx_size << std::endl;
    config_.ctx_size = new_ctx_size;
    server_manager_->set_context_size(new_ctx_size);
    build_menu();
    
    std::string label = (new_ctx_size >= 1024) 
        ? std::to_string(new_ctx_size / 1024) + "K"
        : std::to_string(new_ctx_size);
    show_notification("Context Size Changed", "Lemonade Server context size is now " + label);
}

void TrayApp::on_show_logs() {
    if (config_.log_file.empty()) {
        show_notification("Error", "No log file configured");
        return;
    }
    
#ifdef _WIN32
    // Close existing log viewer if any
    if (log_viewer_process_) {
        TerminateProcess(log_viewer_process_, 0);
        CloseHandle(log_viewer_process_);
        log_viewer_process_ = nullptr;
    }
    
    // Find lemonade-log-viewer.exe in the same directory as this executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    std::string logViewerPath = exeDir + "\\lemonade-log-viewer.exe";
    std::string cmd = "\"" + logViewerPath + "\" \"" + config_.log_file + "\"";
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    
    if (CreateProcessA(
        nullptr,
        const_cast<char*>(cmd.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        log_viewer_process_ = pi.hProcess;
        CloseHandle(pi.hThread);
    } else {
        show_notification("Error", "Failed to open log viewer");
    }
#elif defined(__APPLE__)
    // Kill existing log viewer if any
    if (log_viewer_pid_ > 0) {
        kill(log_viewer_pid_, SIGTERM);
        log_viewer_pid_ = 0;
    }
    
    // Fork and open Terminal.app with tail command
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"tail -f " + config_.log_file + "\"'";
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        exit(0);
    } else if (pid > 0) {
        log_viewer_pid_ = pid;
    }
#else
    // Kill existing log viewer if any
    if (log_viewer_pid_ > 0) {
        kill(log_viewer_pid_, SIGTERM);
        log_viewer_pid_ = 0;
    }
    
    // Fork and open gnome-terminal or xterm
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        std::string cmd = "gnome-terminal -- tail -f '" + config_.log_file + "' || xterm -e tail -f '" + config_.log_file + "'";
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        exit(0);
    } else if (pid > 0) {
        log_viewer_pid_ = pid;
    }
#endif
}

void TrayApp::on_open_documentation() {
    open_url("https://lemonade-server.ai/docs/");
}

void TrayApp::on_open_llm_chat() {
    open_url("http://" + config_.host + ":" + std::to_string(config_.port) + "/#llm-chat");
}

void TrayApp::on_open_model_manager() {
    open_url("http://" + config_.host + ":" + std::to_string(config_.port) + "/#model-management");
}

void TrayApp::on_upgrade() {
    // TODO: Implement upgrade functionality
    std::cout << "Upgrade functionality not yet implemented" << std::endl;
}

void TrayApp::on_quit() {
    std::cout << "Quitting application..." << std::endl;
    shutdown();
}

void TrayApp::shutdown() {
    if (should_exit_) {
        return;  // Already shutting down
    }
    
    should_exit_ = true;
    
    // Only print shutdown message for persistent server commands (serve/run)
    // Don't print for ephemeral commands (list/pull/delete/status/stop)
    if (config_.command == "serve" || config_.command == "run") {
        std::cout << "Shutting down server..." << std::endl;
    }
    
    // Only print debug message if we actually have something to shutdown
    if (server_manager_ || tray_) {
        DEBUG_LOG(this, "Shutting down gracefully...");
    }
    
    // Close log viewer if open
#ifdef _WIN32
    if (log_viewer_process_) {
        TerminateProcess(log_viewer_process_, 0);
        CloseHandle(log_viewer_process_);
        log_viewer_process_ = nullptr;
    }
#else
    if (log_viewer_pid_ > 0) {
        kill(log_viewer_pid_, SIGTERM);
        log_viewer_pid_ = 0;
    }
#endif
    
    // Stop the server
    if (server_manager_) {
        stop_server();
    }
    
    // Stop the tray
    if (tray_) {
        tray_->stop();
    }
}

void TrayApp::open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    int result = system(("open \"" + url + "\"").c_str());
    (void)result;  // Suppress unused variable warning
#else
    int result = system(("xdg-open \"" + url + "\" &").c_str());
    (void)result;  // Suppress unused variable warning
#endif
}

void TrayApp::show_notification(const std::string& title, const std::string& message) {
    if (tray_) {
        tray_->show_notification(title, message);
    }
}

std::string TrayApp::get_loaded_model() {
    try {
        auto health = server_manager_->get_health();
        
        // Check if model is loaded
        if (health.contains("model_loaded") && !health["model_loaded"].is_null()) {
            std::string loaded = health["model_loaded"].get<std::string>();
            if (!loaded.empty()) {
                return loaded;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to get loaded model: " << e.what() << std::endl;
    }
    
    return "";  // No model loaded
}

std::vector<LoadedModelInfo> TrayApp::get_all_loaded_models() {
    std::vector<LoadedModelInfo> loaded_models;
    
    try {
        auto health = server_manager_->get_health();
        
        // Check for all_models_loaded array
        if (health.contains("all_models_loaded") && health["all_models_loaded"].is_array()) {
            for (const auto& model : health["all_models_loaded"]) {
                LoadedModelInfo info;
                info.model_name = model.value("model_name", "");
                info.checkpoint = model.value("checkpoint", "");
                info.last_use = model.value("last_use", 0.0);
                info.type = model.value("type", "llm");
                info.device = model.value("device", "");
                info.backend_url = model.value("backend_url", "");
                
                if (!info.model_name.empty()) {
                    loaded_models.push_back(info);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to get loaded models: " << e.what() << std::endl;
    }
    
    return loaded_models;
}

std::vector<ModelInfo> TrayApp::get_downloaded_models() {
    try {
        auto models_json = server_manager_->get_models();
        std::vector<ModelInfo> models;
        
        // Parse the models JSON response
        // Expected format: {"data": [{"id": "...", "checkpoint": "...", "recipe": "..."}], "object": "list"}
        if (models_json.contains("data") && models_json["data"].is_array()) {
            for (const auto& model : models_json["data"]) {
                ModelInfo info;
                info.id = model.value("id", "");
                info.checkpoint = model.value("checkpoint", "");
                info.recipe = model.value("recipe", "");
                
                if (!info.id.empty()) {
                    models.push_back(info);
                }
            }
        } else {
            DEBUG_LOG(this, "No 'data' array in models response");
        }
        
        return models;
    } catch (const std::exception& e) {
        std::cerr << "Failed to get models: " << e.what() << std::endl;
        return {};
    }
}

void TrayApp::tail_log_to_console() {
    // Wait a bit for the log file to be created
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
#ifdef _WIN32
    HANDLE hFile = CreateFileA(
        config_.log_file.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    
    if (hFile == INVALID_HANDLE_VALUE) {
        return;  // Can't open log file, silently exit
    }
    
    // Seek to end of file
    DWORD currentPos = SetFilePointer(hFile, 0, nullptr, FILE_END);
    
    std::vector<char> buffer(4096);
    
    while (!stop_tail_thread_) {
        // Check if file has grown
        DWORD currentFileSize = GetFileSize(hFile, nullptr);
        if (currentFileSize != INVALID_FILE_SIZE && currentFileSize > currentPos) {
            // File has new data
            SetFilePointer(hFile, currentPos, nullptr, FILE_BEGIN);
            
            DWORD bytesToRead = currentFileSize - currentPos;
            DWORD bytesRead = 0;
            
            while (bytesToRead > 0 && !stop_tail_thread_) {
                DWORD chunkSize = (bytesToRead > buffer.size()) ? buffer.size() : bytesToRead;
                if (ReadFile(hFile, buffer.data(), chunkSize, &bytesRead, nullptr) && bytesRead > 0) {
                    std::cout.write(buffer.data(), bytesRead);
                    std::cout.flush();
                    currentPos += bytesRead;
                    bytesToRead -= bytesRead;
                } else {
                    break;
                }
            }
        }
        
        // Sleep before next check
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    CloseHandle(hFile);
#else
    // Unix implementation (similar logic using FILE*)
    FILE* fp = fopen(config_.log_file.c_str(), "r");
    if (!fp) {
        return;
    }
    
    // Seek to end
    fseek(fp, 0, SEEK_END);
    long currentPos = ftell(fp);
    
    char buffer[4096];
    
    while (!stop_tail_thread_) {
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        
        if (fileSize > currentPos) {
            fseek(fp, currentPos, SEEK_SET);
            size_t bytesToRead = fileSize - currentPos;
            
            while (bytesToRead > 0 && !stop_tail_thread_) {
                size_t chunkSize = (bytesToRead > sizeof(buffer)) ? sizeof(buffer) : bytesToRead;
                size_t bytesRead = fread(buffer, 1, chunkSize, fp);
                if (bytesRead > 0) {
                    std::cout.write(buffer, bytesRead);
                    std::cout.flush();
                    currentPos += bytesRead;
                    bytesToRead -= bytesRead;
                } else {
                    break;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    fclose(fp);
#endif
}

} // namespace lemon_tray



