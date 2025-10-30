#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <sstream>
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/model_manager.h>
#include <lemon/utils/http_client.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace lemon;
using namespace lemon::utils;

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

// Get server info by scanning processes (similar to Python implementation)
std::pair<int, int> get_server_info() {
    // Returns {pid, port} or {0, 0} if not found
#ifdef _WIN32
    // Windows implementation using netstat
    std::string cmd = "netstat -ano | findstr LISTENING";
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return {0, 0};
    
    char buffer[512];
    DWORD current_pid = GetCurrentProcessId();
    
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        
        // Parse netstat output: TCP    0.0.0.0:8000           0.0.0.0:0              LISTENING       1234
        // or: TCP    [::1]:8000             [::]:0                 LISTENING       1234
        std::istringstream iss(line);
        std::string proto, local_addr, foreign_addr, state;
        int pid;
        
        if (!(iss >> proto >> local_addr >> foreign_addr >> state >> pid)) continue;
        if (state != "LISTENING") continue;
        if (pid == (int)current_pid) continue; // Skip self
        
        // Extract port from local address (format: 0.0.0.0:PORT or [::1]:PORT)
        size_t colon_pos = local_addr.rfind(':');
        if (colon_pos == std::string::npos) continue;
        
        try {
            int port = std::stoi(local_addr.substr(colon_pos + 1));
            
            // Check if this PID is lemonade-router.exe
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProcess) {
                char process_name[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameA(hProcess, 0, process_name, &size)) {
                    std::string proc_name(process_name);
                    if (proc_name.find("lemonade-router.exe") != std::string::npos) {
                        CloseHandle(hProcess);
                        _pclose(pipe);
                        return {pid, port};
                    }
                }
                CloseHandle(hProcess);
            }
        } catch (...) {
            // Skip invalid port numbers
            continue;
        }
    }
    _pclose(pipe);
#else
    // Linux/macOS: scan /proc or use lsof
    // For now, just try common ports
    for (int port : {8000, 8001, 8002, 8003}) {
        if (is_server_running("localhost", port)) {
            return {0, port}; // PID not needed for stop command
        }
    }
#endif
    return {0, 0};
}

// Helper: Check if server is running
bool is_server_running(const std::string& host, int port) {
    std::string url = "http://" + host + ":" + std::to_string(port) + "/api/v1/health";
    return HttpClient::is_reachable(url, 2);
}

// Helper: Wait for server to start
bool wait_for_server(const std::string& host, int port, int max_seconds = 10) {
    for (int i = 0; i < max_seconds * 10; ++i) {
        if (is_server_running(host, port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Helper: Send API request
HttpResponse api_request(const std::string& method, const std::string& endpoint, 
                        const std::string& body = "", 
                        const std::string& host = "localhost", int port = 8000) {
    std::string url = "http://" + host + ":" + std::to_string(port) + endpoint;
    
    if (method == "GET") {
        return HttpClient::get(url);
    } else if (method == "POST") {
        std::map<std::string, std::string> headers = {{"Content-Type", "application/json"}};
        return HttpClient::post(url, body, headers);
    }
    
    return {500, "{\"error\": \"Invalid method\"}", {}};
}

int main(int argc, char** argv) {
    try {
        CLIParser parser;
        
        if (!parser.parse(argc, argv)) {
            return 1;
        }
        
        if (parser.should_show_version()) {
            std::cout << "lemon.cpp version 1.0.0" << std::endl;
            return 0;
        }
        
        std::string command = parser.get_command();
        
        if (command == "serve") {
            auto config = parser.get_serve_config();
            
            // Check if server is already running
            auto [pid, running_port] = get_server_info();
            if (running_port != 0) {
                std::cout << "Lemonade Server is already running on port " << running_port << std::endl;
                std::cout << "Please stop the existing server before starting a new instance." << std::endl;
                return 2; // SERVER_ALREADY_RUNNING
            }
            
            Server server(config.port, config.host, config.log_level,
                        config.ctx_size, config.tray, config.llamacpp_backend);
            
            // Register signal handler for Ctrl+C
            g_server_instance = &server;
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            
            server.run();
            
            // Clean up
            g_server_instance = nullptr;
            
        } else if (command == "status") {
            auto [pid, port] = get_server_info();
            
            if (port != 0) {
                std::cout << "Server is running on port " << port << std::endl;
                return 0;
            } else {
                std::cout << "Server is not running" << std::endl;
                return 1;
            }
            
        } else if (command == "stop") {
            auto [pid, port] = get_server_info();
            
            if (port == 0) {
                std::cout << "Lemonade Server is not running" << std::endl;
                return 0;
            }
            
            // Send shutdown request
            std::cout << "Stopping server..." << std::endl;
            try {
                auto response = api_request("POST", "/internal/shutdown", "", "localhost", port);
                if (response.status_code == 200) {
                    std::cout << "Lemonade Server stopped successfully." << std::endl;
                    return 0;
                } else {
                    std::cerr << "Failed to stop server: HTTP " << response.status_code << std::endl;
                    return 1;
                }
            } catch (const std::exception& e) {
                // Connection error is expected when server shuts down
                // Wait a moment and verify the server is actually stopped
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto [check_pid, check_port] = get_server_info();
                if (check_port == 0) {
                    std::cout << "Lemonade Server stopped successfully." << std::endl;
                    return 0;
                } else {
                    std::cerr << "Error stopping server: " << e.what() << std::endl;
                    return 1;
                }
            }
            
        } else if (command == "list") {
            // Check if server is running, start ephemeral if needed
            bool server_was_running = is_server_running("localhost", 8000);
            std::unique_ptr<Server> ephemeral_server;
            std::thread server_thread;
            
            if (!server_was_running) {
                std::cout << "[INFO] Starting ephemeral server..." << std::endl;
                ephemeral_server = std::make_unique<Server>(8000, "localhost", "error");
                server_thread = std::thread([&]() {
                    ephemeral_server->run();
                });
                
                if (!wait_for_server("localhost", 8000)) {
                    std::cerr << "[ERROR] Failed to start ephemeral server" << std::endl;
                    return 1;
                }
            }
            
            // Get models via API (with show_all=true to see all models, not just downloaded)
            auto response = api_request("GET", "/api/v1/models?show_all=true");
            
            if (response.status_code == 200) {
                try {
                    auto models_json = nlohmann::json::parse(response.body);
                    
                    if (!models_json.contains("data") || !models_json["data"].is_array()) {
                        std::cerr << "[ERROR] Invalid response format" << std::endl;
                        std::cerr << "Response: " << response.body.substr(0, 200) << std::endl;
                        return 1;
                    }
                    
                    auto models_array = models_json["data"];
                    
                    // Print header
                    std::cout << std::left 
                              << std::setw(40) << "Model Name"
                              << std::setw(12) << "Downloaded"
                              << "Details" << std::endl;
                    std::cout << std::string(100, '-') << std::endl;
                    
                    // Print each model
                    for (const auto& model : models_array) {
                        // Safely extract fields with defaults
                        std::string name = model.value("name", "unknown");
                        bool is_downloaded = model.value("downloaded", false);
                        std::string status = is_downloaded ? "Yes" : "No";
                        
                        // Format labels
                        std::string details = "-";
                        if (model.contains("labels") && model["labels"].is_array() && !model["labels"].empty()) {
                            details = "";
                            auto labels = model["labels"];
                            for (size_t i = 0; i < labels.size(); ++i) {
                                if (!labels[i].is_null() && labels[i].is_string()) {
                                    details += labels[i].get<std::string>();
                                    if (i < labels.size() - 1) {
                                        details += ", ";
                                    }
                                }
                            }
                        }
                        
                        std::cout << std::left
                                  << std::setw(40) << name
                                  << std::setw(12) << status
                                  << details << std::endl;
                    }
                    
                    std::cout << std::string(100, '-') << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] Failed to parse response: " << e.what() << std::endl;
                    std::cerr << "Response body: " << response.body.substr(0, 500) << std::endl;
                }
            } else {
                std::cerr << "[ERROR] Failed to fetch models (HTTP " << response.status_code << "): " << response.body << std::endl;
            }
            
            // Stop ephemeral server
            if (!server_was_running && ephemeral_server) {
                ephemeral_server->stop();
                if (server_thread.joinable()) {
                    server_thread.join();
                }
            }
            
        } else if (command == "pull") {
            auto config = parser.get_pull_config();
            
            // Check if server is running, start ephemeral if needed
            bool server_was_running = is_server_running("localhost", 8000);
            std::unique_ptr<Server> ephemeral_server;
            std::thread server_thread;
            
            if (!server_was_running) {
                std::cout << "[INFO] Starting ephemeral server..." << std::endl;
                ephemeral_server = std::make_unique<Server>(8000, "localhost", "error");
                server_thread = std::thread([&]() {
                    ephemeral_server->run();
                });
                
                if (!wait_for_server("localhost", 8000)) {
                    std::cerr << "[ERROR] Failed to start ephemeral server" << std::endl;
                    return 1;
                }
            }
            
            // Pull via API
            for (const auto& model_name : config.models) {
                std::cout << "\nPulling model: " << model_name << std::endl;
                
                nlohmann::json request = {{"model_name", model_name}};
                
                // Add optional parameters if provided
                if (!config.checkpoint.empty()) {
                    request["checkpoint"] = config.checkpoint;
                }
                if (!config.recipe.empty()) {
                    request["recipe"] = config.recipe;
                }
                if (config.reasoning) {
                    request["reasoning"] = config.reasoning;
                }
                if (config.vision) {
                    request["vision"] = config.vision;
                }
                if (!config.mmproj.empty()) {
                    request["mmproj"] = config.mmproj;
                }
                
                auto response = api_request("POST", "/api/v1/pull", request.dump());
                
                if (response.status_code == 200) {
                    std::cout << "[SUCCESS] Model pulled: " << model_name << std::endl;
                } else {
                    std::cerr << "[ERROR] Failed to pull " << model_name << ": " << response.body << std::endl;
                }
            }
            
            // Stop ephemeral server
            if (!server_was_running && ephemeral_server) {
                ephemeral_server->stop();
                if (server_thread.joinable()) {
                    server_thread.join();
                }
            }
            
        } else if (command == "delete") {
            auto config = parser.get_delete_config();
            
            // Check if server is running, start ephemeral if needed
            bool server_was_running = is_server_running("localhost", 8000);
            std::unique_ptr<Server> ephemeral_server;
            std::thread server_thread;
            
            if (!server_was_running) {
                std::cout << "[INFO] Starting ephemeral server..." << std::endl;
                ephemeral_server = std::make_unique<Server>(8000, "localhost", "error");
                server_thread = std::thread([&]() {
                    ephemeral_server->run();
                });
                
                if (!wait_for_server("localhost", 8000)) {
                    std::cerr << "[ERROR] Failed to start ephemeral server" << std::endl;
                    return 1;
                }
            }
            
            // Delete via API
            for (const auto& model_name : config.models) {
                std::cout << "\nDeleting model: " << model_name << std::endl;
                
                nlohmann::json request = {{"model", model_name}};
                auto response = api_request("POST", "/api/v1/delete", request.dump());
                
                if (response.status_code == 200) {
                    std::cout << "[SUCCESS] Model deleted: " << model_name << std::endl;
                } else {
                    std::cerr << "[ERROR] Failed to delete " << model_name << ": " << response.body << std::endl;
                }
            }
            
            // Stop ephemeral server
            if (!server_was_running && ephemeral_server) {
                ephemeral_server->stop();
                if (server_thread.joinable()) {
                    server_thread.join();
                }
            }
            
        } else if (command == "run") {
            // TODO: Implement run command
            std::cout << "Run command not yet implemented" << std::endl;
            
        } else {
            std::cerr << "Unknown command: " << command << std::endl;
            return 1;
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

