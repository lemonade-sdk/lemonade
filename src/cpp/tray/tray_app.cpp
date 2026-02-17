#include "lemon_tray/tray_app.h"
#ifdef _WIN32
#include "lemon_tray/platform/windows_tray.h"  // For set_menu_update_callback
#endif
#include "LemonadeServiceManager.h"  // For macOS service management
#include <lemon/single_instance.h>
#include <lemon/system_info.h>
#include <lemon/version.h>
#include <lemon/utils/path_utils.h>
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
#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#endif
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>  // for _NSGetExecutablePath
#endif

namespace fs = std::filesystem;

namespace lemon_tray {

// Helper function to detect if a path is a local filesystem path
// Returns true for absolute paths (Windows: C:\... or D:\..., Unix: /...)
static bool is_local_path(const std::string& path) {
    if (path.empty()) return false;

    // Windows absolute path: C:\... or D:\... (also handles forward slashes)
    if (path.length() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }

    // Unix absolute path: /...
    if (path[0] == '/') {
        return true;
    }

    return false;
}

#if !defined(_WIN32)
// Helper: Check if systemd is running and a unit is active
static bool is_systemd_service_active(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    if (!unit_name || unit_name[0] == '\0') {
        return false;
    }

    if (sd_booted() <= 0) {
        return false;
    }

    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        sd_bus_error_free(&error);
        return false;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager",
        "GetUnit",
        &error,
        &reply,
        "s",
        unit_name
    );

    if (r < 0 || !reply) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    const char* unit_path = nullptr;
    r = sd_bus_message_read(reply, "o", &unit_path);
    sd_bus_message_unref(reply);
    reply = nullptr;

    if (r < 0 || !unit_path) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    char* active_state = nullptr;
    r = sd_bus_get_property_string(
        bus,
        "org.freedesktop.systemd1",
        unit_path,
        "org.freedesktop.systemd1.Unit",
        "ActiveState",
        &error,
        &active_state
    );

    if (r < 0 || !active_state) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }

    bool is_active =
        (strcmp(active_state, "active") == 0) ||
        (strcmp(active_state, "activating") == 0) ||
        (strcmp(active_state, "reloading") == 0);

    free(active_state);
    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    return is_active;
#else
    (void)unit_name;
    return false;
#endif
}

// Helper: Get systemd service MainPID (returns 0 if unavailable)
static int get_systemd_service_main_pid(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    if (!unit_name || unit_name[0] == '\0') {
        return 0;
    }

    if (sd_booted() <= 0) {
        return 0;
    }

    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        sd_bus_error_free(&error);
        return 0;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager",
        "GetUnit",
        &error,
        &reply,
        "s",
        unit_name
    );

    if (r < 0 || !reply) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    const char* unit_path = nullptr;
    r = sd_bus_message_read(reply, "o", &unit_path);
    sd_bus_message_unref(reply);
    reply = nullptr;

    if (r < 0 || !unit_path) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    unsigned int main_pid = 0;
    r = sd_bus_get_property_trivial(
        bus,
        "org.freedesktop.systemd1",
        unit_path,
        "org.freedesktop.systemd1.Service",
        "MainPID",
        &error,
        'u',
        &main_pid
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return 0;
    }

    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    return static_cast<int>(main_pid);
#else
    (void)unit_name;
    return 0;
#endif
}

// Helper: Check if systemd service is active in another process (not this one)
static bool is_systemd_service_active_other_process(const char* unit_name) {
#ifdef HAVE_SYSTEMD
    if (!is_systemd_service_active(unit_name)) {
        return false;
    }

    int main_pid = get_systemd_service_main_pid(unit_name);
    if (main_pid <= 0) {
        return false;
    }

    return (main_pid != getpid());
#else
    (void)unit_name;
    return false;
#endif
}

// Helper: Known systemd unit names for lemonade server (native or snap)
static const char* kSystemdUnitNames[] = {
    "lemonade-server.service",
    "snap.lemonade-server.daemon.service"
};

static const char* get_active_systemd_unit_name() {
    for (const auto* unit_name : kSystemdUnitNames) {
        if (is_systemd_service_active(unit_name)) {
            return unit_name;
        }
    }
    return nullptr;
}

static bool is_any_systemd_service_active() {
    return get_active_systemd_unit_name() != nullptr;
}

static int get_systemd_any_service_main_pid() {
    const char* unit_name = get_active_systemd_unit_name();
    if (!unit_name) {
        return 0;
    }
    return get_systemd_service_main_pid(unit_name);
}

static bool is_systemd_any_service_active_other_process() {
    for (const auto* unit_name : kSystemdUnitNames) {
        if (is_systemd_service_active_other_process(unit_name)) {
            return true;
        }
    }
    return false;
}
#endif

// Helper macro for debug logging
#define DEBUG_LOG(app, msg) \
    if ((app)->server_config_.log_level == "debug") { \
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

TrayApp::TrayApp(const lemon::ServerConfig& server_config, const lemon::TrayConfig& tray_config)
    : current_version_(LEMON_VERSION_STRING)
    , should_exit_(false)
    , server_config_(server_config)
    , tray_config_(tray_config)
#ifdef _WIN32
    , electron_app_process_(nullptr)
    , electron_job_object_(nullptr)
#else
    , electron_app_pid_(0)
#endif
{
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

TrayApp::~TrayApp() {
    // Stop signal monitor thread if running
#ifndef _WIN32
    if (signal_monitor_thread_.joinable()) {
        stop_signal_monitor_ = true;
        signal_monitor_thread_.join();
    }
#endif

    // Only shutdown if we actually started something
    if (server_manager_ || !tray_config_.command.empty()) {
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
    DEBUG_LOG(this, "TrayApp::run() starting...");
    DEBUG_LOG(this, "Command: " << tray_config_.command);

    bool server_already_running = false;
    bool run_command_already_executed = false;

    // Find server binary automatically (needed for most commands)
    if (server_binary_.empty()) {
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

    DEBUG_LOG(this, "Using server binary: " << server_binary_);

    // Handle commands
    if (tray_config_.command == "list") {
        return execute_list_command();
    } else if (tray_config_.command == "pull") {
        return execute_pull_command();
    } else if (tray_config_.command == "delete") {
        return execute_delete_command();
    } else if (tray_config_.command == "status") {
        return execute_status_command();
    } else if (tray_config_.command == "stop") {
        return execute_stop_command();
    } else if (tray_config_.command == "recipes") {
        return execute_recipes_command();
    } else if (tray_config_.command == "serve" || tray_config_.command == "run") {
        auto connect_to_running_server = [this, &server_already_running, &run_command_already_executed](const char* context) -> int {
            std::cout << "Lemonade Server is " << context << " already running. Connecting to it..." << std::endl;

            auto [pid, running_port] = get_server_info();
            (void)pid;
            if (running_port == 0) {
                std::cerr << "Error: Could not connect to running server" << std::endl;
                return 1;
            }

            server_manager_ = std::make_unique<ServerManager>(server_config_.host, running_port);
            server_config_.port = running_port;

            server_already_running = true;

            if (server_config_.host.empty() || server_config_.host == "0.0.0.0") {
                server_config_.host = "localhost";
            }

            if (tray_config_.command == "run") {
                int result = execute_run_command();
                if (result != 0) {
                    return result;
                }
                run_command_already_executed = true;
            }

            return 0;
        };

#ifdef HAVE_SYSTEMD
        if (tray_config_.command == "run" &&
            is_systemd_any_service_active_other_process()) {
            int result = connect_to_running_server("managed by systemd and");
            if (result != 0) {
                return result;
            }
            // Continue to tray initialization below
        }
#endif

        // Check for single instance - only for 'serve' and 'run' commands
        // Other commands (status, list, pull, delete, stop) can run alongside a server
        if (lemon::SingleInstance::IsAnotherInstanceRunning("Server")) {
            // If 'run' command and server is already running, connect to it and execute the run command
            if (tray_config_.command == "run") {
                int result = connect_to_running_server("");
                if (result != 0) {
                    return result;
                }
                // Continue to tray initialization below
            } else {
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
        }
        // Continue to server initialization below
    } else if (tray_config_.command == "tray") {
        // Check for single instance - prevent duplicate tray processes,
        // the use case here is not for a tray launched by the system its when its being launched by the user or electron app.
        #ifdef __APPLE__
        if (LemonadeServiceManager::isTrayActive())
        {
            std::cout << "Lemonade Tray is already running." << std::endl;
            return 0;
        }
        // isTrayActive won't work for when the tray is launched as a service, that requires a bunch of other permissions
        // Permissions that have to be entered into a plist and its beyond the scope of this PR, maybe TODO?
        if (lemon::SingleInstance::IsAnotherInstanceRunning("Tray")) {
            std::cout << "Lemonade Tray is already running." << std::endl;
            return 0;
        }
        #else
        if (lemon::SingleInstance::IsAnotherInstanceRunning("Tray")) {
            std::cout << "Lemonade Tray is already running." << std::endl;
            return 0;
        }
        #endif

#ifdef __APPLE__
        // macOS: Tray mode - show tray, starting server service if necessary
        // Check if server service is already running
        if (lemon::SingleInstance::IsAnotherInstanceRunning("Server") || LemonadeServiceManager::isServerActive()) {
            // Server is already running - get its port and connect
            auto [pid, running_port] = get_server_info();
            bool server_was_running = (running_port != 0);

            if (running_port != 0) {
                std::cout << "Connected to Lemonade Server on port " << running_port << std::endl;
                // Create server manager to communicate with running server
                // Use localhost to connect (works regardless of what the server is bound to)
                if (server_config_.host.empty() || server_config_.host == "0.0.0.0") {
                    server_config_.host = "localhost";
                }
                server_manager_ = std::make_unique<ServerManager>(server_config_.host, running_port);
                server_manager_->set_port(running_port);
                server_config_.port = running_port;  // Update config to match running server
            } else {
                std::cerr << "Error: Server service is active but no port found: " << running_port << std::endl;
                return 1;
            }
        } else {
            // Server is not running - start the service
            std::cout << "Starting Lemonade Server service..." << std::endl;
            LemonadeServiceManager::startServer();

            // Wait for the service to start (up to 30 seconds)
            int max_wait = 5;
            for (int i = 0; i < max_wait; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto [pid, running_port] = get_server_info();
                if (running_port != 0) {
                    std::cout << "Server service started on port " << running_port << std::endl;

                    // Create server manager to communicate with running server
                    server_manager_ = std::make_unique<ServerManager>(server_config_.host, server_config_.port);
                    server_manager_->set_port(running_port);
                    server_config_.port = running_port;  // Update config to match running server

                    // Use localhost to connect (works regardless of what the server is bound to)
                    if (server_config_.host.empty() || server_config_.host == "0.0.0.0") {
                        server_config_.host = "localhost";
                    }

                    // Continue to tray initialization below
                    break;
                }
                if (i == max_wait - 1) {
                    std::cerr << "Error: Server service failed to start within 30 seconds" << std::endl;
                    return 1;
                }
            }
        }
#else
        // Other platforms: Tray-only mode - just show tray connected to existing server
        // Check if server is already running
        auto [pid, running_port] = get_server_info();
        if (running_port == 0) {
            std::cerr << "Error: No Lemonade Server is currently running.\n"
                      << "Start the server first with: lemonade-server serve\n"
                      << "Or run: lemonade-server serve --no-tray" << std::endl;
            return 1;
        }

        // Create server manager to communicate with running server
        server_manager_ = std::make_unique<ServerManager>(server_config_.host, server_config_.port);
        server_manager_->set_port(running_port);
        server_config_.port = running_port;  // Update config to match running server

        // Use localhost to connect (works regardless of what the server is bound to)
        if (server_config_.host.empty() || server_config_.host == "0.0.0.0") {
            server_config_.host = "localhost";
        }

        std::cout << "Connected to Lemonade Server on port " << running_port << std::endl;

        // Continue to tray initialization below (skip server startup)
#endif
    } else {
        std::cerr << "Internal Error: Unhandled command '" << tray_config_.command << "'\n" << std::endl;
        return 1;
    }

    if (!server_already_running && tray_config_.command != "tray") {
        // Create server manager
        DEBUG_LOG(this, "Creating server manager...");
        server_manager_ = std::make_unique<ServerManager>(server_config_.host, server_config_.port);

        // Start server
        DEBUG_LOG(this, "Starting server...");
        if (!start_server()) {
            std::cerr << "Error: Failed to start server" << std::endl;
            return 1;
        }

        DEBUG_LOG(this, "Server started successfully!");
        if (tray_config_.command == "serve" && tray_config_.save_options) {
            tray_config_.save_options = false;
            std::cerr << "Warning: Argument --save-options only available for the run command. Ignoring.\n";
        }

        process_owns_server_ = true;
    }

    // If this is the 'run' command, load the model and run electron app
    if (tray_config_.command == "run" && !run_command_already_executed) {
        int result = execute_run_command();
        if (result != 0) {
            return result;
        }
    }

    // If no-tray mode, just wait for server to exit
    if (tray_config_.no_tray) {
        std::cout << "Press Ctrl+C to stop" << std::endl;

#ifdef _WIN32
        // Windows: simple sleep loop (signal handler handles Ctrl+C via console_ctrl_handler)
        if (server_already_running) {
            while (!should_exit_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            while (server_manager_->is_server_running()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
#else
        // Linux: monitor signal pipe using select() for proper signal handling
        while (server_already_running || server_manager_->is_server_running()) {
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

            if (!server_already_running && !server_manager_->is_server_running()) {
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
    tray_->set_log_level(server_config_.log_level);

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
            refresh_menu();
        });
    }
#endif

    // Find icon path (matching the CMake resources structure)
    DEBUG_LOG(this, "Searching for icon...");
    std::string icon_path;

#ifdef __APPLE__
    // On macOS, look for icon in /Library/Application Support/lemonade/resources
    icon_path = "/Library/Application Support/lemonade/resources/static/favicon.ico";
    DEBUG_LOG(this, "Checking macOS Application Support icon at: " << icon_path);

    if (!fs::exists(icon_path)) {
        std::cout << "WARNING: Icon not found at /Library/Application Support/lemonade/resources/favicon.ico, will use default icon" << std::endl;
    } else {
        DEBUG_LOG(this, "Icon found at: " << icon_path);
    }
#else
    // On other platforms, find icon file path
    icon_path = "resources/static/favicon.ico";
    DEBUG_LOG(this, "Checking icon at: " << fs::absolute(icon_path).string());


    if (!fs::exists(icon_path)) {
        // Try relative to executable directory
        fs::path exe_path = fs::path(server_binary_).parent_path();
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
#endif

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
        #ifdef __APPLE__
        auto last_tick = std::chrono::steady_clock::now();
        #endif
        while (!stop_signal_monitor_ && !should_exit_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(signal_pipe_[0], &readfds);

            struct timeval tv = {0, 100000};  // 100ms timeout
            int result = select(signal_pipe_[0] + 1, &readfds, nullptr, nullptr, &tv);
            #ifdef __APPLE__
            // Check if 5 seconds have passed, refresh menu
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_tick).count() >= 5) {

                DEBUG_LOG(this, "Checking if menu needs refresh");
                refresh_menu();

                // Reset the tracker
                last_tick = now;
            }
            #endif
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
    //Initialize thread to constantly update the tray models
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
    bool got_exe_path = false;

#ifdef __APPLE__
    // macOS: Use _NSGetExecutablePath
    uint32_t bufsize = sizeof(exe_path_buf);
    if (_NSGetExecutablePath(exe_path_buf, &bufsize) == 0) {
        got_exe_path = true;
    }
#else
    // Linux: Use /proc/self/exe
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len != -1) {
        exe_path_buf[len] = '\0';
        got_exe_path = true;
    }
#endif

    if (got_exe_path) {
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
    search_paths.push_back("/opt/bin/" + binary_name);
    search_paths.push_back("/usr/bin/" + binary_name);
#endif

    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            server_binary_ = fs::absolute(path).string();
            DEBUG_LOG(this, "Found server binary: " << server_binary_);
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
    if (is_any_systemd_service_active()) {
        int main_pid = get_systemd_any_service_main_pid();
        if (main_pid > 0) {
            return {main_pid, server_config_.port};
        }
    }

    // Unix: Read from PID file
    std::ifstream pid_file("/tmp/lemonade-router.pid");
    if (pid_file.is_open()) {
        int pid, port;
        pid_file >> pid >> port;
        pid_file.close();

        // Verify the PID is still alive
        if (getpgid(pid) != -1) {
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
        server_manager_ = std::make_unique<ServerManager>(server_config_.host, port);
    }

    DEBUG_LOG(this, "Starting ephemeral server on port " << port << "...");

    bool success = server_manager_->start_server(
        server_binary_,
        port,
        server_config_.recipe_options,
        log_file_.empty() ? "" : log_file_,
        server_config_.log_level,  // Pass log level to ServerManager
        false,  // show_console - SSE streaming provides progress via client
        true,   // is_ephemeral (suppress startup message)
        server_config_.host,  // Pass host to ServerManager
        server_config_.max_loaded_models,
        server_config_.extra_models_dir  // Pass extra models directory
    );

    if (!success) {
        std::cerr << "Failed to start ephemeral server" << std::endl;
        return false;
    }

    return true;
}

int TrayApp::server_call(std::function<int(std::unique_ptr<ServerManager> const &)> to_call) {
    // Check if server is running
    auto [pid, running_port] = get_server_info();
    bool server_was_running = (running_port != 0);
    int port = server_was_running ? running_port : server_config_.port;

    // Start ephemeral server if needed
    if (!server_was_running) {
        if (!start_ephemeral_server(port)) {
            return 1;
        }
    }

    if (!server_manager_) {
        server_manager_ = std::make_unique<ServerManager>(server_config_.host, port);
    }

    int res = to_call(server_manager_);

    // Stop ephemeral server
    if (!server_was_running) {
        DEBUG_LOG(this, "Stopping ephemeral server...");
        stop_server();
    }

    return res;
}

// Command: list
int TrayApp::execute_list_command() {
    DEBUG_LOG(this, "Listing available models...");

    // Get models from server with show_all=true to include download status
    return server_call([](std::unique_ptr<ServerManager> const &server_manager) {
        try {
            // Request with show_all=true to get download status
            std::string response = server_manager->make_http_request("/api/v1/models?show_all=true");
            auto models_json = nlohmann::json::parse(response);

            if (!models_json.contains("data") || !models_json["data"].is_array()) {
                std::cerr << "Invalid response format from server" << std::endl;
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
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error listing models: " << e.what() << std::endl;
            return 1;
        }
    });
}

// Command: pull
int TrayApp::execute_pull_command() {
    // Track if this is a local import (affects how we call the server)
    bool local_import = false;

    // Check if checkpoint is a local filesystem path
    if (!tray_config_.checkpoint.empty() && is_local_path(tray_config_.checkpoint)) {
        // Validate path exists
        if (!fs::exists(tray_config_.checkpoint)) {
            std::cerr << "Error: Local path does not exist: " << tray_config_.checkpoint << std::endl;
            return 1;
        }

        // Validate model name has user. prefix for local imports
        if (tray_config_.model.substr(0, 5) != "user.") {
            std::cerr << "Error: When importing from a local path, model name must start with 'user.'" << std::endl;
            std::cerr << "Example: lemonade-server pull user.MyModel --checkpoint C:\\models\\my-model --recipe llamacpp" << std::endl;
            return 1;
        }

        // Recipe is required for local imports
        if (tray_config_.recipe.empty()) {
            std::cerr << "Error: --recipe is required when importing from a local path" << std::endl;
            std::cerr << "Options: llamacpp, ryzenai-llm, whispercpp" << std::endl;
            return 1;
        }

        std::cout << "Importing model from local path: " << tray_config_.checkpoint << std::endl;

        // Get HF cache directory (same logic as ModelManager)
        std::string hf_cache;
        if (const char* env = std::getenv("HF_HUB_CACHE")) {
            hf_cache = env;
        } else if (const char* env = std::getenv("HF_HOME")) {
            hf_cache = std::string(env) + "/hub";
        } else {
#ifdef _WIN32
            if (const char* userprofile = std::getenv("USERPROFILE")) {
                hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
            }
#else
            if (const char* home = std::getenv("HOME")) {
                hf_cache = std::string(home) + "/.cache/huggingface/hub";
            }
#endif
        }

        // Copy files to HF cache
        std::string model_name_clean = tray_config_.model.substr(5); // Remove "user." prefix
        std::replace(model_name_clean.begin(), model_name_clean.end(), '/', '-');
        std::string dest_path = hf_cache + "/models--" + model_name_clean;

        std::cout << "Copying files to: " << dest_path << std::endl;
        fs::create_directories(dest_path);

        fs::path src_path(tray_config_.checkpoint);
        if (fs::is_directory(src_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(src_path)) {
                fs::path relative = fs::relative(entry.path(), src_path);
                fs::path dest_file = fs::path(dest_path) / relative;
                if (entry.is_directory()) {
                    fs::create_directories(dest_file);
                } else {
                    fs::create_directories(dest_file.parent_path());
                    fs::copy_file(entry.path(), dest_file, fs::copy_options::overwrite_existing);
                }
            }
        } else {
            fs::copy_file(src_path, fs::path(dest_path) / src_path.filename(),
                         fs::copy_options::overwrite_existing);
        }

        local_import = true;
    }

    std::cout << (local_import ? "Registering model: " : "Pulling model: ") << tray_config_.model << std::endl;

    return server_call([&](std::unique_ptr<ServerManager> const &server_manager) {
        // Pull model via API (SSE streaming for downloads, simple POST for local imports)
        try {
            // Build request body with all optional parameters
            // Local imports don't need streaming (no download progress)
            nlohmann::json request_body = {{"model", tray_config_.model}, {"stream", !local_import}};

            if (local_import) {
                request_body["local_import"] = true;
            }
            if (!tray_config_.checkpoint.empty() && !local_import) {
                // Only send checkpoint for remote downloads (local files already copied)
                request_body["checkpoint"] = tray_config_.checkpoint;
            }
            if (!tray_config_.recipe.empty()) {
                request_body["recipe"] = tray_config_.recipe;
            }
            if (tray_config_.is_reasoning) {
                request_body["reasoning"] = true;
            }
            if (tray_config_.is_vision) {
                request_body["vision"] = true;
            }
            if (tray_config_.is_embedding) {
                request_body["embedding"] = true;
            }
            if (tray_config_.is_reranking) {
                request_body["reranking"] = true;
            }
            if (!tray_config_.mmproj.empty()) {
                request_body["mmproj"] = tray_config_.mmproj;
            }

            httplib::Client cli = server_manager->make_http_client(86400, 30);

            // For local imports, use simple POST (no SSE streaming needed)
            if (local_import) {
                auto res = cli.Post("/api/v1/pull", request_body.dump(), "application/json");

                if (!res) {
                    throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
                }

                if (res->status != 200) {
                    try {
                        auto error_json = nlohmann::json::parse(res->body);
                        throw std::runtime_error(error_json.value("error", res->body));
                    } catch (const nlohmann::json::exception&) {
                        throw std::runtime_error("Server returned status " + std::to_string(res->status));
                    }
                }

                std::cout << "Model imported successfully: " << tray_config_.model << std::endl;

                return 0;
            }

            // Use SSE streaming to receive progress events (for remote downloads)
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
                std::cout << "Model pulled successfully: " << tray_config_.model << std::endl;
            } else if (!res) {
                // Connection closed without success - this is an error
                throw std::runtime_error("Connection closed unexpectedly");
            } else {
                std::cerr << "Pull completed without success confirmation" << std::endl;
                return 1;
            }

            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error pulling model: " << e.what() << std::endl;
            return 1;
        }
    });
}

// Command: delete
int TrayApp::execute_delete_command() {
    std::cout << "Deleting model: " << tray_config_.model << std::endl;

    return server_call([&](std::unique_ptr<ServerManager> const &server_manager) {
        // Delete model via API
        try {
            nlohmann::json request_body = {{"model", tray_config_.model}};
            std::string response = server_manager->make_http_request(
                "/api/v1/delete",
                "POST",
                request_body.dump()
            );

            auto response_json = nlohmann::json::parse(response);
            if (response_json.value("status", "") == "success") {
                std::cout << "Model deleted successfully: " << tray_config_.model << std::endl;
            } else {
                std::cerr << "Failed to delete model" << std::endl;
                return 1;
            }

            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error deleting model: " << e.what() << std::endl;
            return 1;
        }
    });
}

// Command: run
int TrayApp::execute_run_command() {
    std::cout << "Running model: " << tray_config_.model << std::endl;

    // The run command will:
    // 1. Start server (already done in main run() before this function is called)
    // 2. Load the model
    // 3. Open browser
    // 4. Show tray (handled by main run() after this returns)

    // Note: Server is already started and ready - start_server() does health checks internally

    // Load the model
    std::cout << "Loading model " << tray_config_.model << "..." << std::endl;
    if (server_manager_->load_model(tray_config_.model, server_config_.recipe_options, tray_config_.save_options)) {
        std::cout << "Model loaded successfully!" << std::endl;

        // Launch the Electron app
        bool should_launch_app = process_owns_server_;
#ifdef HAVE_SYSTEMD
    if (!should_launch_app && is_any_systemd_service_active()) {
            should_launch_app = true;
        }
#endif
        if (should_launch_app) {
            std::cout << "Launching Lemonade app..." << std::endl;
            launch_electron_app();
        }
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

// Command: recipes
int TrayApp::execute_recipes_command() {
    auto statuses = lemon::SystemInfo::get_all_recipe_statuses();

    // Print table header
    std::cout << std::left << std::setw(20) << "Recipe"
              << std::setw(12) << "Backend"
              << std::setw(14) << "Status"
              << "Version/Error" << std::endl;
    std::cout << std::string(75, '-') << std::endl;

    for (const auto& status : statuses) {
        bool first_backend = true;

        if (status.backends.empty()) {
            // Recipe with no backends (shouldn't happen, but handle gracefully)
            std::cout << std::left << std::setw(20) << status.name
                      << std::setw(12) << "-"
                      << std::setw(14) << (status.supported ? "supported" : "unsupported")
                      << "-" << std::endl;
        } else {
            for (const auto& backend : status.backends) {
                // Recipe name only on first line
                std::string recipe_col = first_backend ? status.name : "";

                // Determine status string
                // Check supported first - unsupported takes precedence even if installed
                std::string status_str;
                if (!backend.supported) {
                    status_str = "unsupported";
                } else if (backend.available) {
                    status_str = "installed";
                } else {
                    status_str = "supported";
                }

                // Version or error (concise)
                std::string info_col;
                if (!backend.version.empty() && backend.version != "unknown") {
                    info_col = backend.version;
                } else if (!backend.supported && !backend.error.empty()) {
                    info_col = backend.error;
                } else {
                    info_col = "-";
                }

                std::cout << std::left << std::setw(20) << recipe_col
                          << std::setw(12) << backend.name
                          << std::setw(14) << status_str
                          << info_col << std::endl;

                first_backend = false;
            }
        }
    }

    std::cout << std::string(75, '-') << std::endl;
    return 0;
}

// Command: stop
int TrayApp::execute_stop_command() {
    auto [pid, port] = get_server_info();

    if (port == 0) {
        std::cout << "Lemonade Server is not running" << std::endl;
        return 0;
    }

    // On Linux, check if server is managed by systemd and warn user
#ifndef _WIN32
    const char* active_systemd_unit = get_active_systemd_unit_name();
    if (active_systemd_unit) {
        std::cerr << "Error: Lemonade Server is managed by systemd." << std::endl;
        std::string unit_name(active_systemd_unit);
        std::cerr << "Please use: sudo systemctl stop " << unit_name << std::endl;
        std::cerr << "Instead of: lemonade-server stop" << std::endl;
        return 1;
    }
#endif

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
    if (log_file_.empty()) {
        #ifdef _WIN32
        // Windows: %TEMP%\lemonade-server.log
        char* temp_path = nullptr;
        size_t len = 0;
        _dupenv_s(&temp_path, &len, "TEMP");
        if (temp_path) {
            log_file_ = std::string(temp_path) + "\\lemonade-server.log";
            free(temp_path);
        } else {
            log_file_ = "lemonade-server.log";
        }
        #else
        // Unix: /tmp/lemonade-server.log or ~/.lemonade/server.log
        log_file_ = "/tmp/lemonade-server.log";
        #endif
        DEBUG_LOG(this, "Using default log file: " << log_file_);
    }

    bool success = server_manager_->start_server(
        server_binary_,
        server_config_.port,
        server_config_.recipe_options,
        log_file_,
        server_config_.log_level,  // Pass log level to ServerManager
        true,               // Always show console output for serve command
        false,              // is_ephemeral = false (persistent server, show startup message with URL)
        server_config_.host,        // Pass host to ServerManager
        server_config_.max_loaded_models,
        server_config_.extra_models_dir  // Pass extra models directory
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

    // Cache current state for refresh comparisons
    last_menu_loaded_models_ = get_all_loaded_models();
    last_menu_available_models_ = get_downloaded_models();
}

void TrayApp::refresh_menu() {
    if (!tray_) return;

    // Only refresh if something has actually changed
    if (menu_needs_refresh()) {
        DEBUG_LOG(this, "Menu state changed, rebuilding menu");
        build_menu();
    }
}

bool TrayApp::menu_needs_refresh() {
    // Check if loaded models have changed
    auto current_loaded = get_all_loaded_models();
    if (current_loaded != last_menu_loaded_models_) {
        return true;
    }

    // Check if available models have changed
    auto current_available = get_downloaded_models();
    if (current_available != last_menu_available_models_) {
        return true;
    }

    // Could add more checks here for other dynamic content

    return false;
}

Menu TrayApp::create_menu() {
    Menu menu;

    // Open app - at the very top (Electron app takes priority, web app as fallback)
    if (electron_app_path_.empty()) {
        // Try to find the Electron app if we haven't already
        const_cast<TrayApp*>(this)->find_electron_app();
    }
    if (!electron_app_path_.empty()) {
        // Electron app is available - use it
        menu.add_item(MenuItem::Action("Open Lemonade App", [this]() { launch_electron_app(); }));
        menu.add_separator();
    } else {
        // Electron app not available - check for web app as fallback
        const_cast<TrayApp*>(this)->find_web_app();
        if (web_app_available_) {
            menu.add_item(MenuItem::Action("Open Lemonade App", [this]() { const_cast<TrayApp*>(this)->open_web_app(); }));
            menu.add_separator();
        }
    }

    // Get loaded model once and cache it to avoid redundant health checks
    std::string loaded = is_loading_model_ ? "" : get_loaded_model();
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

#ifdef __APPLE__
    // Service Control menu items (macOS only)
    bool service_running = LemonadeServiceManager::isServerActive();
    bool service_enabled = LemonadeServiceManager::isServerEnabled();

    // Show Start/Stop Service based on running state
    if (service_running) {
        menu.add_item(MenuItem::Action("Stop Service", [this]() {
            LemonadeServiceManager::stopServer();
            build_menu();
        }));
    } else if(service_enabled) {
        menu.add_item(MenuItem::Action("Start Service", [this]() {
            LemonadeServiceManager::startServer();
            build_menu();
        }));
    }

    // Show Enable/Disable Service based on enabled state
    if (service_enabled) {
        menu.add_item(MenuItem::Action("Disable Service", [this]() {
            LemonadeServiceManager::disableServer();
            build_menu();
        }));
    } else {
        menu.add_item(MenuItem::Action("Enable Service", [this]() {
            LemonadeServiceManager::enableServer();
            build_menu();
        }));
    }
#endif

    // Port submenu
    auto port_submenu = std::make_shared<Menu>();
    std::vector<int> ports = {8000, 8020, 8040, 8060, 8080, 9000};
    for (int port : ports) {
        bool is_current = (port == server_config_.port);
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
        {"32K", 32768}, {"64K", 65536}, {"128K", 131072},
        {"256K", 262144}
    };
    for (const auto& [label, size] : ctx_sizes) {
        bool is_current = (size == server_config_.recipe_options["ctx_size"]);
        ctx_submenu->add_item(MenuItem::Checkable(
            "Context size " + label,
            [this, size = size]() { on_change_context_size(size); },
            is_current
        ));
    }
    menu.add_item(MenuItem::Submenu("Context Size", ctx_submenu));

    menu.add_separator();

    // Main menu items
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));
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
    server_config_.port = new_port;
    server_manager_->set_port(new_port);
    build_menu();
    show_notification("Port Changed", "Lemonade Server is now running on port " + std::to_string(new_port));
}

void TrayApp::on_change_context_size(int new_ctx_size) {
    std::cout << "Changing context size to: " << new_ctx_size << std::endl;
    server_config_.recipe_options["ctx_size"] = new_ctx_size;
    server_manager_->set_context_size(new_ctx_size);
    build_menu();

    std::string label = (new_ctx_size >= 1024)
        ? std::to_string(new_ctx_size / 1024) + "K"
        : std::to_string(new_ctx_size);
    show_notification("Context Size Changed", "Lemonade Server context size is now " + label);
}

void TrayApp::on_show_logs() {
    if (log_file_.empty()) {
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
    std::string cmd = "\"" + logViewerPath + "\" \"" + log_file_ + "\"";

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
    // Use open command to open log file in default editor instead of Terminal.app
    std::string cmd = "open \"" + log_file_ + "\"";
    int result = system(cmd.c_str());
    if (result != 0) {
        show_notification("Error", "Failed to open log file");
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
        std::string cmd = "gnome-terminal -- tail -f '" + log_file_ + "' || xterm -e tail -f '" + log_file_ + "'";
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

void TrayApp::on_upgrade() {
    // TODO: Implement upgrade functionality
    std::cout << "Upgrade functionality not yet implemented" << std::endl;
}

void TrayApp::send_unload_command() {
#ifdef __APPLE__
    // On macOS, send HTTP unload command directly to the server
    if (server_manager_) {
        try {
            std::cout << "Sending unload command to server..." << std::endl;
            server_manager_->make_http_request("/api/v1/unload", "POST", "", 30);
            std::cout << "Unload command sent successfully" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to send unload command: " << e.what() << std::endl;
        }
    }
#endif
}

void TrayApp::on_quit() {
    std::cout << "Quitting application..." << std::endl;
#ifdef __APPLE__
    // On macOS, disable the service first before quitting
    std::cout << "Disabling service auto-start..." << std::endl;
    LemonadeServiceManager::performFullQuit();

    // Send UnloadModels command to the lemonade server
    send_unload_command();
#endif
    shutdown();
}

void TrayApp::shutdown() {
    if (should_exit_) {
        return;  // Already shutting down
    }

    should_exit_ = true;

    // Only print shutdown message if we started the server
    if (process_owns_server_) {
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
    if (server_manager_ && process_owns_server_) {
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

bool TrayApp::find_electron_app() {
    // Get directory of this executable (lemonade-tray)
    fs::path exe_dir;


#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    exe_dir = fs::path(exe_path).parent_path();
#else
    char exe_path[PATH_MAX];
    bool got_exe_path = false;

#ifdef __APPLE__
    // macOS: Use _NSGetExecutablePath
    uint32_t bufsize = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &bufsize) == 0) {
        got_exe_path = true;
    }
#else
    // Linux: Use /proc/self/exe
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        got_exe_path = true;
    }
#endif

    if (!got_exe_path) {
        return false;
    }
    exe_dir = fs::path(exe_path).parent_path();
#endif

    // The Electron app has exactly two possible locations:
    // 1. Production (WIX installer): ../app/ relative to bin/ directory
    // 2. Development: app/<platform>-unpacked/ relative to build directory
    // 3. Linux production: /opt/share/lemonade-server/app/lemonade

#ifdef _WIN32
    constexpr const char* exe_name = "Lemonade.exe";
    constexpr const char* unpacked_dir = "win-unpacked";
#elif defined(__APPLE__)
    constexpr const char* exe_name = "Lemonade.app";
    constexpr const char* unpacked_dir = "mac";
#else
    constexpr const char* exe_name = "lemonade";
    constexpr const char* unpacked_dir = "linux-unpacked";
#endif

#if defined(__linux__)
    // On Linux, check the production installation path first
    // If the executable is in /opt/bin, the app is in /opt/share/lemonade-server/app/
    if (exe_dir == "/opt/bin") {
        fs::path linux_production_path = fs::path("/opt/share/lemonade-server/app") / exe_name;
        if (fs::exists(linux_production_path)) {
            electron_app_path_ = fs::canonical(linux_production_path).string();
            std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
            return true;
        }
    }
#endif

    // Check production path first (most common case)
    // WiX installer puts binaries in bin/ and app in app/, so ../app/ from bin/
    fs::path production_path = exe_dir / ".." / "app" / exe_name;
    if (fs::exists(production_path)) {
        electron_app_path_ = fs::canonical(production_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }

#ifdef __APPLE__
    // macOS: Check standard installation location
    fs::path mac_apps_path = fs::path("/Applications") / exe_name;
    if (fs::exists(mac_apps_path)) {
        electron_app_path_ = fs::canonical(mac_apps_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }
#endif
    // Check development path (app/<platform>-unpacked/ in build directory)
    // CMake builds to build/Release/ on Windows, electron app to build/app/
    // So we need ../app/<platform>-unpacked/ from build/Release/
    fs::path dev_path = exe_dir / ".." / "app" / unpacked_dir / exe_name;
    if (fs::exists(dev_path)) {
        electron_app_path_ = fs::canonical(dev_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }

    // Legacy development path (same directory as tray executable - for backwards compatibility)
    fs::path legacy_dev_path = exe_dir / exe_name;
    if (fs::exists(legacy_dev_path)) {
        electron_app_path_ = fs::canonical(legacy_dev_path).string();
        std::cout << "Found Electron app at: " << electron_app_path_ << std::endl;
        return true;
    }

    std::cerr << "Warning: Could not find Electron app" << std::endl;
    std::cerr << "  Checked: " << production_path.string() << std::endl;
    std::cerr << "  Checked: " << dev_path.string() << std::endl;
    std::cerr << "  Checked: " << legacy_dev_path.string() << std::endl;
    return false;
}

bool TrayApp::find_web_app() {
    // Use the same path resolution as the server
    std::string web_app_dir = lemon::utils::get_resource_path("resources/web-app");

    // Check if web app directory exists and has an index.html
    if (fs::exists(web_app_dir) && fs::is_directory(web_app_dir)) {
        fs::path index_path = fs::path(web_app_dir) / "index.html";
        if (fs::exists(index_path)) {
            std::cout << "Found web app at: " << web_app_dir << std::endl;
            web_app_available_ = true;
            return true;
        }
    }

    web_app_available_ = false;
    return false;
}

void TrayApp::open_web_app() {
    // Compose the web app URL
    // Translate 0.0.0.0 to localhost since 0.0.0.0 is not a valid connect address
    std::string connect_host = server_config_.host;
    if (connect_host.empty() || connect_host == "0.0.0.0") {
        connect_host = "localhost";
    }
    std::string web_app_url = "http://" + connect_host + ":" + std::to_string(server_config_.port) + "/";
    std::cout << "Opening web app at: " << web_app_url << std::endl;
    open_url(web_app_url);
}

void TrayApp::launch_electron_app() {
    // Try to find the app if we haven't already
    if (electron_app_path_.empty()) {
        if (!find_electron_app()) {
            std::cerr << "Error: Cannot launch Electron app - not found" << std::endl;
            return;
        }
    }

    // Compose the server base URL for the Electron app
    // Translate 0.0.0.0 to localhost since 0.0.0.0 is not a valid connect address
    std::string connect_host = server_config_.host;
    if (connect_host.empty() || connect_host == "0.0.0.0") {
        connect_host = "localhost";
    }
    std::string base_url = "http://" + connect_host + ":" + std::to_string(server_config_.port);
    std::cout << "Launching Electron app with server URL: " << base_url << std::endl;

#ifdef _WIN32
    // Single-instance enforcement: Only allow one Electron app to be open at a time
    // Reuse child process tracking to determine if the app is already running
    if (electron_app_process_ != nullptr) {
        // Check if the process is still alive
        DWORD exit_code = 0;
        if (GetExitCodeProcess(electron_app_process_, &exit_code) && exit_code == STILL_ACTIVE) {
            std::cout << "Electron app is already running" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, clean up the handle
            CloseHandle(electron_app_process_);
            electron_app_process_ = nullptr;
        }
    }
#endif

    // Launch the Electron app
#ifdef _WIN32
    // Windows: Create a job object to ensure the Electron app closes when tray closes
    if (!electron_job_object_) {
        electron_job_object_ = CreateJobObjectA(NULL, NULL);
        if (electron_job_object_) {
            // Configure job to terminate all processes when the last handle is closed
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
            job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (!SetInformationJobObject(
                electron_job_object_,
                JobObjectExtendedLimitInformation,
                &job_info,
                sizeof(job_info))) {
                std::cerr << "Warning: Failed to configure job object: " << GetLastError() << std::endl;
                CloseHandle(electron_job_object_);
                electron_job_object_ = nullptr;
            } else {
                std::cout << "Created job object for Electron app process management" << std::endl;
            }
        } else {
            std::cerr << "Warning: Failed to create job object: " << GetLastError() << std::endl;
        }
    }

    // Launch the .exe with
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Build command line: "path\to\Lemonade.exe"
    // Note: CreateProcessA modifies the command line buffer, so we need a mutable copy
    std::string cmd_line = "\"" + electron_app_path_ + "\"";
    std::vector<char> cmd_line_buf(cmd_line.begin(), cmd_line.end());
    cmd_line_buf.push_back('\0');

    // Create the process
    if (CreateProcessA(
        NULL,                         // Application name (NULL = use command line)
        cmd_line_buf.data(),          // Command line with arguments
        NULL,                         // Process security attributes
        NULL,                         // Thread security attributes
        FALSE,                        // Don't inherit handles
        CREATE_SUSPENDED,             // Create suspended so we can add to job before it runs
        NULL,                         // Environment
        NULL,                         // Current directory
        &si,                          // Startup info
        &pi))                         // Process info
    {
        // Add the process to the job object if we have one
        if (electron_job_object_) {
            if (AssignProcessToJobObject(electron_job_object_, pi.hProcess)) {
                std::cout << "Added Electron app to job object (will close with tray)" << std::endl;
            } else {
                std::cerr << "Warning: Failed to add process to job object: " << GetLastError() << std::endl;
            }
        }

        // Resume the process now that it's in the job object
        ResumeThread(pi.hThread);

        // Store the process handle (don't close it - we need it for cleanup)
        electron_app_process_ = pi.hProcess;
        CloseHandle(pi.hThread);  // We don't need the thread handle

        std::cout << "Launched Electron app" << std::endl;
    } else {
        std::cerr << "Failed to launch Electron app: " << GetLastError() << std::endl;
    }
#elif defined(__APPLE__)
    // Single-instance enforcement: Check if the Electron app is already running
    if (electron_app_pid_ > 0) {
        // Check if the process is still alive
        if (kill(electron_app_pid_, 0) == 0) {
            std::cout << "Electron app is already running (PID: " << electron_app_pid_ << ")" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, reset the PID
            electron_app_pid_ = 0;
        }
    }

    // macOS: Use 'open' command to launch the .app with --args to pass arguments
    // Note: 'open' doesn't give us the PID directly, so we'll need to find it
    std::string cmd = "open \"" + electron_app_path_ + "\"";
    int result = system(cmd.c_str());
    if (result == 0) {
        std::cout << "Launched Electron app" << std::endl;

        // Try to find the PID of the Electron app we just launched
        // Look for process named "Lemonade" (the app name, not the .app bundle name)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Give it time to start
        FILE* pipe = popen("pgrep -n Lemonade", "r");  // -n = newest matching process
        if (pipe) {
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                electron_app_pid_ = atoi(buffer);
                std::cout << "Tracking Electron app (PID: " << electron_app_pid_ << ")" << std::endl;
            }
            pclose(pipe);
        }
    } else {
        std::cerr << "Failed to launch Electron app" << std::endl;
    }
#else
    // Single-instance enforcement: Check if the Electron app is already running
    if (electron_app_pid_ > 0) {
        // Check if the process is still alive (and not a zombie)
        if (is_process_alive_not_zombie(electron_app_pid_)) {
            std::cout << "Electron app is already running (PID: " << electron_app_pid_ << ")" << std::endl;
            show_notification("App Already Running", "The Lemonade app is already open");
            return;
        } else {
            // Process has exited, reset the PID
            electron_app_pid_ = 0;
        }
    }

    // Linux: Launch the binary directly using fork/exec for proper PID tracking
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: execute the Electron app
        execl(electron_app_path_.c_str(), electron_app_path_.c_str(), nullptr);
        // If execl returns, it failed
        std::cerr << "Failed to execute Electron app: " << strerror(errno) << std::endl;
        _exit(1);
    } else if (pid > 0) {
        // Parent process: store the PID
        electron_app_pid_ = pid;
        std::cout << "Launched Electron app (PID: " << electron_app_pid_ << ")" << std::endl;
    } else {
        // Fork failed
        std::cerr << "Failed to launch Electron app: " << strerror(errno) << std::endl;
    }
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
        log_file_.c_str(),
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
    FILE* fp = fopen(log_file_.c_str(), "r");
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
