#include "lemon_server/tray_app.h"
#include "lemon_server/platform/windows_tray.h"  // For set_menu_update_callback
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

namespace lemon_server {

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
#ifdef _WIN32
    , electron_app_process_(nullptr)
    , electron_job_object_(nullptr)
#else
    , electron_app_pid_(0)
#endif
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
        config_.max_reranking_models,
        config_.max_audio_models
    );

    if (!success) {
        std::cerr << "Failed to start ephemeral server" << std::endl;
        return false;
    }
    
    return true;
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
        config_.max_reranking_models,
        config_.max_audio_models
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
    
    // Close Electron app if open
#ifdef _WIN32
    if (electron_app_process_) {
        // The job object will automatically terminate the process when we close it
        // But we can optionally terminate it gracefully first
        CloseHandle(electron_app_process_);
        electron_app_process_ = nullptr;
    }
    if (electron_job_object_) {
        // Closing the job object will terminate all processes in it
        CloseHandle(electron_job_object_);
        electron_job_object_ = nullptr;
    }
#else
    // macOS/Linux: Terminate the Electron app if it's running
    if (electron_app_pid_ > 0) {
        if (is_process_alive_not_zombie(electron_app_pid_)) {
            std::cout << "Terminating Electron app (PID: " << electron_app_pid_ << ")..." << std::endl;
            kill(electron_app_pid_, SIGTERM);
            
            // Wait briefly for graceful shutdown
            for (int i = 0; i < 10; i++) {
                if (!is_process_alive_not_zombie(electron_app_pid_)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            // Force kill if still alive
            if (is_process_alive_not_zombie(electron_app_pid_)) {
                std::cout << "Force killing Electron app..." << std::endl;
                kill(electron_app_pid_, SIGKILL);
            }
        }
        electron_app_pid_ = 0;
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

} // namespace lemon_server
