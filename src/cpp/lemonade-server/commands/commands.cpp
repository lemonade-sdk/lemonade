// Command implementations for lemonade-server CLI
// These are TrayApp methods extracted to a separate file for organization.

#include "lemon_server/tray_app.h"
#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

namespace lemon_server {

// Helper macro for debug logging (matches tray_app.cpp)
#define DEBUG_LOG(app, msg) \
    if ((app)->config_.log_level == "debug") { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

// ============================================================
// Command: list
// ============================================================

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

// ============================================================
// Command: pull
// ============================================================

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

// ============================================================
// Command: delete
// ============================================================

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

// ============================================================
// Command: run
// ============================================================

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
        
        // Launch the Electron app
        std::cout << "Launching Lemonade app..." << std::endl;
        launch_electron_app();
    } else {
        std::cerr << "Failed to load model" << std::endl;
        return 1;
    }
    
    // Return success - main run() will continue to tray initialization or wait loop
    return 0;
}

// ============================================================
// Command: status
// ============================================================

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

// ============================================================
// Command: stop
// ============================================================

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
        bool router_gone = !fs::exists("/proc/" + std::to_string(router_pid));
        bool tray_gone = (tray_pid == 0 || !fs::exists("/proc/" + std::to_string(tray_pid)));
        
        // Check if all children have exited
        bool all_children_gone = true;
        for (int child_pid : router_children) {
            if (fs::exists("/proc/" + std::to_string(child_pid))) {
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
        if (fs::exists("/proc/" + std::to_string(router_pid))) {
            std::cout << "Force killing router (PID: " << router_pid << ")" << std::endl;
            kill(router_pid, SIGKILL);
        }
        
        // Force kill tray app if it exists
        if (tray_pid != 0 && fs::exists("/proc/" + std::to_string(tray_pid))) {
            std::cout << "Force killing tray app (PID: " << tray_pid << ")" << std::endl;
            kill(tray_pid, SIGKILL);
        }
        
        // Force kill any remaining children (matching Python's behavior for stubborn llama-server)
        if (!router_children.empty()) {
            for (int child_pid : router_children) {
                if (fs::exists("/proc/" + std::to_string(child_pid))) {
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

} // namespace lemon_server

