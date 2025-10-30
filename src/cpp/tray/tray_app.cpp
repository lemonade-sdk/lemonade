#include "lemon_tray/tray_app.h"
#include "lemon_tray/platform/windows_tray.h"  // For set_menu_update_callback
#include <lemon/single_instance.h>
#include <httplib.h>
#include <iostream>
#include <iomanip>
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
    : current_version_("1.0.0")  // TODO: Load from version file
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
    
    // Find server binary if not specified (needed for most commands)
    if (config_.server_binary.empty()) {
        DEBUG_LOG(this, "Searching for server binary...");
        if (!find_server_binary()) {
            std::cerr << "Error: Could not find lemonade server binary" << std::endl;
            std::cerr << "Please specify --server-binary path" << std::endl;
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
    } else if (config_.command == "run") {
        return execute_run_command();
    } else if (config_.command == "status") {
        return execute_status_command();
    } else if (config_.command == "stop") {
        return execute_stop_command();
    } else if (config_.command == "serve") {
        // Check for single instance - only for 'serve' command
        // Other commands (status, list, pull, delete, stop) can run alongside a server
        if (lemon::SingleInstance::IsAnotherInstanceRunning("ServerBeta")) {
            std::cerr << "Error: Another instance of lemonade-server-beta serve is already running.\n"
                      << "Only one persistent server can run at a time.\n\n"
                      << "To check server status: lemonade-server-beta status\n"
                      << "To stop the server: lemonade-server-beta stop\n" << std::endl;
            return 1;
        }
        // Continue to serve logic below
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
    
    // If no-tray mode, just wait for server to exit
    if (config_.no_tray) {
        std::cout << "Server running in foreground mode (no tray)" << std::endl;
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
    
    // Set ready callback
    DEBUG_LOG(this, "Setting ready callback...");
    tray_->set_ready_callback([this]() {
        DEBUG_LOG(this, "Ready callback triggered!");
        show_notification("Woohoo!", "Lemonade Server is running! Right-click the tray icon to access options.");
    });
    
    // Set menu update callback to refresh state before showing menu
    DEBUG_LOG(this, "Setting menu update callback...");
    if (auto* windows_tray = dynamic_cast<WindowsTray*>(tray_.get())) {
        windows_tray->set_menu_update_callback([this]() {
            DEBUG_LOG(this, "Refreshing menu state from server...");
            build_menu();
        });
    }
    
    // Find icon path (matching the CMake resources structure)
    DEBUG_LOG(this, "Searching for icon...");
    std::string icon_path = "resources/static/favicon.ico";
    std::cout << "DEBUG: Checking icon at: " << fs::absolute(icon_path).string() << std::endl;
    
    if (!fs::exists(icon_path)) {
        // Try relative to executable directory
        fs::path exe_path = fs::path(config_.server_binary).parent_path();
        icon_path = (exe_path / "resources" / "static" / "favicon.ico").string();
        std::cout << "DEBUG: Icon not found, trying: " << icon_path << std::endl;
        
        // If still not found, try without static subdir (fallback)
        if (!fs::exists(icon_path)) {
            icon_path = (exe_path / "resources" / "favicon.ico").string();
            std::cout << "DEBUG: Icon not found, trying fallback: " << icon_path << std::endl;
        }
    }
    
    if (fs::exists(icon_path)) {
        std::cout << "DEBUG: Icon found at: " << icon_path << std::endl;
    } else {
        std::cout << "WARNING: Icon not found at any location, will use default icon" << std::endl;
    }
    
    // Initialize tray
    std::cout << "DEBUG: Initializing tray with icon: " << icon_path << std::endl;
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
            } else if (arg == "--server-binary" && i + 1 < argc) {
                config_.server_binary = argv[++i];
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
    std::cout << "  --log-file PATH          Log file path\n";
    std::cout << "  --server-binary PATH     Path to lemonade-router binary\n";
    std::cout << "  --no-tray                Start server without tray (headless mode)\n";
    std::cout << "  --help, -h               Show this help message\n";
    std::cout << "  --version, -v            Show version\n\n";
    std::cout << "Examples:\n";
    std::cout << "  lemonade-server-beta serve                        # Start server with tray\n";
    std::cout << "  lemonade-server-beta serve --port 8080            # Start on custom port\n";
    std::cout << "  lemonade-server-beta serve --no-tray              # Start without tray\n";
    std::cout << "  lemonade-server-beta list                         # List models\n";
    std::cout << "  lemonade-server-beta pull Llama-3.2-1B-Instruct-CPU   # Download a model\n";
    std::cout << "  lemonade-server-beta run Llama-3.2-1B-Instruct-CPU    # Run a model\n";
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
    std::string binary_name = "lemonade";
    
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
    // Unix: Parse netstat or use similar approach
    // For now, check common ports as fallback
    for (int port : {8000, 8001, 8002, 8003, 8020, 8040, 8060, 8080}) {
        try {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(0, 200000);
            cli.set_read_timeout(0, 300000);
            
            auto res = cli.Get("/api/v1/health");
            if (res && res->status == 200) {
                return {0, port};
            }
        } catch (...) {
        }
    }
#endif
    
    return {0, 0};  // Server not found
}

// Helper: Start ephemeral server
bool TrayApp::start_ephemeral_server(int port) {
    if (!server_manager_) {
        server_manager_ = std::make_unique<ServerManager>();
    }
    
    std::cout << "[INFO] Starting ephemeral server on port " << port << "..." << std::endl;
    
    bool success = server_manager_->start_server(
        config_.server_binary,
        port,
        config_.ctx_size,
        config_.log_file.empty() ? "" : config_.log_file
    );
    
    if (!success) {
        std::cerr << "[ERROR] Failed to start ephemeral server" << std::endl;
        return false;
    }
    
    return true;
}

// Command: list
int TrayApp::execute_list_command() {
    std::cout << "Listing available models..." << std::endl;
    
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
    
    // Get models from server
    try {
        if (!server_manager_) {
            server_manager_ = std::make_unique<ServerManager>();
        }
        
        auto models_json = server_manager_->get_models();
        
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
            // TODO: Check if downloaded
            std::string downloaded = "?";
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
        stop_server();
    }
    
    return 0;
}

// Command: pull
int TrayApp::execute_pull_command() {
    if (config_.command_args.empty()) {
        std::cerr << "Error: model name required" << std::endl;
        std::cerr << "Usage: lemonade-server-beta pull <model_name>" << std::endl;
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
    
    // TODO: Implement pull via API
    std::cout << "Pull functionality will be implemented via API endpoint" << std::endl;
    
    // Stop ephemeral server
    if (!server_was_running) {
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
    
    // TODO: Implement delete via API
    std::cout << "Delete functionality will be implemented via API endpoint" << std::endl;
    
    // Stop ephemeral server
    if (!server_was_running) {
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
    
    // Check if server is already running
    auto [pid, running_port] = get_server_info();
    if (running_port != 0) {
        std::cout << "Server is already running on port " << running_port << std::endl;
        // TODO: Load the model and open browser
        return 0;
    }
    
    // Start persistent server (with tray)
    std::cout << "Starting server..." << std::endl;
    if (!start_server()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    
    // Load the model
    std::cout << "Loading model " << model_name << "..." << std::endl;
    if (server_manager_->load_model(model_name)) {
        std::cout << "Model loaded successfully!" << std::endl;
        // TODO: Open browser to chat interface
        std::string url = "http://localhost:" + std::to_string(config_.port) + "/?model=" + model_name + "#llm-chat";
        std::cout << "You can now chat with " << model_name << " at " << url << std::endl;
        open_url(url);
    } else {
        std::cerr << "Failed to load model" << std::endl;
    }
    
    // If no-tray mode, wait for server
    if (config_.no_tray) {
        std::cout << "Server running in foreground mode (no tray)" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
        while (server_manager_->is_server_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } else {
        // Start tray interface
        return TrayApp::run();  // This will show the tray
    }
    
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
#else
    // Unix: Kill processes by name
    system("pkill -f lemonade-router");
    system("pkill -f 'lemonade-server-beta.*serve'");
#endif
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
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
        std::cout << "Using default log file: " << config_.log_file << std::endl;
    }
    
    return server_manager_->start_server(
        config_.server_binary,
        config_.port,
        config_.ctx_size,
        config_.log_file
    );
}

void TrayApp::stop_server() {
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
    
    // Status display
    std::string loaded = get_loaded_model();
    if (!loaded.empty()) {
        menu.add_item(MenuItem::Action("Loaded: " + loaded, nullptr, false));
        menu.add_item(MenuItem::Action("Unload LLM", [this]() { on_unload_model(); }));
    } else {
        menu.add_item(MenuItem::Action("No models loaded", nullptr, false));
    }
    
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
            bool is_loaded = (model.id == loaded);
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
    std::cout << "Loading model: " << model_name << std::endl;
    if (server_manager_->load_model(model_name)) {
        loaded_model_ = model_name;
        build_menu();
    }
}

void TrayApp::on_unload_model() {
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
    // Open new PowerShell window with tail-like command
    // Use Start-Process to open a new window that stays open
    std::string cmd = "powershell -Command \"Start-Process powershell -ArgumentList '-NoExit','-Command',\\\"Get-Content -Wait '" + config_.log_file + "'\\\"\"";
    system(cmd.c_str());
#elif defined(__APPLE__)
    // Open Terminal.app with tail command
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"tail -f " + config_.log_file + "\"'";
    system(cmd.c_str());
#else
    // Linux: try gnome-terminal or xterm
    std::string cmd = "gnome-terminal -- tail -f '" + config_.log_file + "' || xterm -e tail -f '" + config_.log_file + "'";
    system(cmd.c_str());
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
        std::cout << "Shutting down gracefully..." << std::endl;
    }
    
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
            std::cout << "DEBUG: Found " << models_json["data"].size() << " models from server" << std::endl;
            
            for (const auto& model : models_json["data"]) {
                ModelInfo info;
                info.id = model.value("id", "");
                info.checkpoint = model.value("checkpoint", "");
                info.recipe = model.value("recipe", "");
                
                if (!info.id.empty()) {
                    std::cout << "DEBUG: Added model: " << info.id << std::endl;
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

} // namespace lemon_tray


