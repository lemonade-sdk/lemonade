#include "lemon_tray/tray_app.h"
#include "lemon_tray/platform/windows_tray.h"  // For set_menu_update_callback
#include <lemon/single_instance.h>
#include <lemon/version.h>
#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>

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
#include <unistd.h>  // for readlink
#endif

namespace fs = std::filesystem;

namespace lemon_tray {

// Helper macro for debug logging
#define DEBUG_LOG(app, msg) \
    if ((app)->config_.log_level == "debug") { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

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
        
        if (g_tray_app_instance) {
            g_tray_app_instance->shutdown();
        }
        
        return TRUE;  // We handled it
    }
    return FALSE;
}
#else
// Unix signal handler
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived interrupt signal, shutting down gracefully..." << std::endl;
        
        if (g_tray_app_instance) {
            g_tray_app_instance->shutdown();
        }
        
        exit(0);
    }
}
#endif

TrayApp::TrayApp(int argc, char* argv[])
    : current_version_(LEMON_VERSION_STRING)
    , should_exit_(false)
{
    parse_arguments(argc, argv);
    
    if (config_.show_help) {
        print_usage();
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
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
#endif
        
        DEBUG_LOG(this, "Signal handlers installed");
    }
}

TrayApp::~TrayApp() {
    // Only shutdown if we actually started something
    if (server_manager_ || !config_.command.empty()) {
        shutdown();
    }
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
        if (lemon::SingleInstance::IsAnotherInstanceRunning("ServerBeta")) {
#ifdef _WIN32
            show_simple_notification("Server Already Running", "Lemonade Server is already running");
#endif
            std::cerr << "Error: Another instance of lemonade-server-beta serve/run is already running.\n"
                      << "Only one persistent server can run at a time.\n\n"
                      << "To check server status: lemonade-server-beta status\n"
                      << "To stop the server: lemonade-server-beta stop\n" << std::endl;
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
        
        // TODO: Set up signal handlers for Ctrl+C
        while (server_manager_->is_server_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
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
    
    DEBUG_LOG(this, "Menu built, entering event loop...");
    // Run tray event loop
    tray_->run();
    
    DEBUG_LOG(this, "Event loop exited");
    return 0;
}

void TrayApp::parse_arguments(int argc, char* argv[]) {
    // First check for --help or --version flags
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
    
    // Check if there's a command (non-flag argument)
    if (argc > 1 && argv[1][0] != '-') {
        config_.command = argv[1];
        
        // Parse remaining arguments (both command args and options)
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--log-level" && i + 1 < argc) {
                config_.log_level = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                config_.port = std::stoi(argv[++i]);
            } else if (arg == "--ctx-size" && i + 1 < argc) {
                config_.ctx_size = std::stoi(argv[++i]);
            } else if (arg == "--llamacpp" && i + 1 < argc) {
                config_.llamacpp_backend = argv[++i];
            } else if (arg == "--no-tray") {
                config_.no_tray = true;
            } else {
                // It's a command argument (like model name)
                config_.command_args.push_back(arg);
            }
        }
        return;
    }
    
    // No command provided - this is an error
    if (argc == 1) {
        config_.command = "";  // Empty command signals error
        return;
    }
    
    // If we get here, we have flags but no command - also an error
    config_.command = "";
}

void TrayApp::print_usage() {
    std::cout << "lemonade-server-beta - Lemonade Server Beta\n\n";
    std::cout << "Usage: lemonade-server-beta <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  serve                    Start the server (default if no command specified)\n";
    std::cout << "  list                     List available models\n";
    std::cout << "  pull <model>             Download a model\n";
    std::cout << "  delete <model>           Delete a model\n";
    std::cout << "  run <model>              Run a model (starts server if needed)\n";
    std::cout << "  status                   Check server status\n";
    std::cout << "  stop                     Stop the server\n\n";
    std::cout << "Serve Options:\n";
    std::cout << "  --port PORT              Server port (default: 8000)\n";
    std::cout << "  --host HOST              Server host (default: localhost)\n";
    std::cout << "  --ctx-size SIZE          Context size (default: 4096)\n";
    std::cout << "  --llamacpp BACKEND       LlamaCpp backend: vulkan, rocm, metal (default: vulkan)\n";
    std::cout << "  --log-file PATH          Log file path\n";
    std::cout << "  --log-level LEVEL        Log level: info, debug, trace (default: info)\n";
#if defined(__linux__) && !defined(__ANDROID__)
    std::cout << "  --no-tray                Start server without tray (default on Linux)\n";
#else
    std::cout << "  --no-tray                Start server without tray (headless mode)\n";
#endif
    std::cout << "  --help, -h               Show this help message\n";
    std::cout << "  --version, -v            Show version\n";
}

void TrayApp::print_version() {
    std::cout << "lemonade-server-beta version " << current_version_ << std::endl;
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

// Helper: Wait for server to be ready
bool TrayApp::wait_for_server_ready(int port, int timeout_seconds) {
    auto server_mgr = std::make_unique<ServerManager>();
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        try {
            auto health = server_mgr->get_health();
            return true;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return false;
}

// Helper: Get server info (returns {pid, port} or {0, 0} if not found)
std::pair<int, int> TrayApp::get_server_info() {
    // Query OS for listening TCP connections and find lemonade-router.exe
#ifdef _WIN32
    // Windows: Use GetExtendedTcpTable to find listening connections
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    
    std::vector<BYTE> buffer(size);
    PMIB_TCPTABLE_OWNER_PID pTcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    
    if (GetExtendedTcpTable(pTcpTable, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
            DWORD pid = pTcpTable->table[i].dwOwningPid;
            int port = ntohs((u_short)pTcpTable->table[i].dwLocalPort);
            
            // Check if this PID is lemonade-router.exe
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProcess) {
                WCHAR processName[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, processName, &size)) {
                    std::wstring fullPath(processName);
                    std::wstring exeName = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
                    
                    if (exeName == L"lemonade-router.exe") {
                        CloseHandle(hProcess);
                        return {static_cast<int>(pid), port};
                    }
                }
                CloseHandle(hProcess);
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
        config_.llamacpp_backend  // Pass llamacpp backend to ServerManager
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
        std::cerr << "Usage: lemonade-server-beta pull <model_name> [--checkpoint CHECKPOINT] [--recipe RECIPE] [--reasoning] [--vision] [--mmproj MMPROJ]" << std::endl;
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
    
    // Pull model via API with optional parameters
    try {
        if (!server_manager_) {
            server_manager_ = std::make_unique<ServerManager>();
        }
        server_manager_->set_port(port);  // Use the detected or configured port
        
        // Build request body with all optional parameters
        nlohmann::json request_body = {{"model", model_name}};
        
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
            } else if (arg == "--mmproj" && i + 1 < config_.command_args.size()) {
                request_body["mmproj"] = config_.command_args[++i];
            }
        }
        
        // Use 2 hour timeout for pull - large models can take a very long time
        std::string response = server_manager_->make_http_request(
            "/api/v1/pull", 
            "POST", 
            request_body.dump(),
            7200  // 2 hours
        );
        
        auto response_json = nlohmann::json::parse(response);
        if (response_json.value("status", "") == "success") {
            std::cout << "Model pulled successfully: " << model_name << std::endl;
        } else {
            std::cerr << "Failed to pull model" << std::endl;
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
        std::cerr << "Usage: lemonade-server-beta delete <model_name>" << std::endl;
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
        std::cerr << "Usage: lemonade-server-beta run <model_name>" << std::endl;
        return 1;
    }
    
    std::string model_name = config_.command_args[0];
    std::cout << "Running model: " << model_name << std::endl;
    
    // The run command will:
    // 1. Start server (handled by main run() after this returns)
    // 2. Wait for server to be ready
    // 3. Load the model
    // 4. Open browser
    // 5. Show tray (handled by main run() after this returns)
    
    // Wait for server to be ready
    std::cout << "Waiting for server to be ready..." << std::endl;
    if (!wait_for_server_ready(config_.port, 30)) {
        std::cerr << "Server did not become ready in time" << std::endl;
        return 1;
    }
    
    // Load the model
    std::cout << "Loading model " << model_name << "..." << std::endl;
    if (server_manager_->load_model(model_name)) {
        std::cout << "Model loaded successfully!" << std::endl;
        
        // Open browser to chat interface
        std::string url = "http://localhost:" + std::to_string(config_.port) + "/?model=" + model_name + "#llm-chat";
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
    
    // Try graceful shutdown via API
    try {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        
        auto res = client.Post("/api/v1/halt");
        
        if (res && (res->status == 200 || res->status == 204)) {
            // Wait a moment for server to shut down
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    } catch (...) {
        // API call failed, try force kill below
    }
    
    // Kill any remaining lemonade-server-beta.exe and lemonade-router.exe processes
    // This handles both the router and the tray app
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                std::wstring process_name(pe32.szExeFile);
                if (process_name == L"lemonade-router.exe" || process_name == L"lemonade-server-beta.exe") {
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProcess) {
                        TerminateProcess(hProcess, 0);
                        CloseHandle(hProcess);
                    }
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }
    
    // Note: log-viewer.exe auto-exits when parent process dies, no need to explicitly kill it
#else
    // Unix: Kill processes by name
    system("pkill -f lemonade-router");
    system("pkill -f 'lemonade-server-beta.*serve'");
    // Kill llama-server child processes (launched by lemonade-router)
    system("pkill -f 'llama-server.*--port'");
    // Kill log viewer processes
    system("pkill -f 'tail -f.*lemonade-server.log'");
    
    // Wait for lock and PID files to be released (critical for clean restarts)
    std::string lock_file = "/tmp/lemonade_ServerBeta.lock";
    std::string pid_file = "/tmp/lemonade-router.pid";
    
    // Poll for up to 10 seconds for files to be released
    bool files_released = false;
    for (int i = 0; i < 100; i++) {  // 100 * 100ms = 10 seconds
        bool lock_exists = fs::exists(lock_file);
        bool pid_exists = fs::exists(pid_file);
        
        if (!lock_exists && !pid_exists) {
            files_released = true;
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!files_released) {
        std::cerr << "Warning: Lock/PID files not released within timeout" << std::endl;
    }
#endif
    
#ifndef _WIN32
    // Unix: no additional sleep needed, we already waited for lock files
#else
    std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
    
    // Verify it stopped
    auto [check_pid, check_port] = get_server_info();
    if (check_port == 0) {
        std::cout << "Lemonade Server stopped successfully." << std::endl;
        return 0;
    }
    
    std::cerr << "Failed to stop server" << std::endl;
    return 1;
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
        true                // Always show console output for serve command
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
    
    // Get loaded model once and cache it to avoid redundant health checks
    std::string loaded = is_loading_model_ ? "" : get_loaded_model();
    
    // Status display
    if (is_loading_model_) {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(loading_mutex_));
        menu.add_item(MenuItem::Action("Loading: " + loading_model_name_ + "...", nullptr, false));
    } else {
        if (!loaded.empty()) {
            menu.add_item(MenuItem::Action("Loaded: " + loaded, nullptr, false));
            menu.add_item(MenuItem::Action("Unload LLM", [this]() { on_unload_model(); }));
        } else {
            menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
        }
    }
    
    // Load Model submenu
    auto load_submenu = std::make_shared<Menu>();
    auto models = get_downloaded_models();
    // Reuse cached value instead of calling get_loaded_model() again
    std::string current_loaded = loaded;
    if (models.empty()) {
        load_submenu->add_item(MenuItem::Action(
            "No models available: Use the Model Manager",
            nullptr,
            false
        ));
    } else {
        for (const auto& model : models) {
            bool is_loaded = (model.id == current_loaded);
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
    
    std::cout << "Unloading model" << std::endl;
    if (server_manager_->unload_model()) {
        loaded_model_.clear();
        build_menu();
    }
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
    open_url("http://localhost:" + std::to_string(config_.port) + "/#llm-chat");
}

void TrayApp::on_open_model_manager() {
    open_url("http://localhost:" + std::to_string(config_.port) + "/#model-management");
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
    
    // Only print shutdown message if we actually have something to shutdown
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
    system(("open \"" + url + "\"").c_str());
#else
    system(("xdg-open \"" + url + "\" &").c_str());
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


