#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <lemon/cli_parser.h>
#include <lemon/server.h>
#include <lemon/model_manager.h>
#include <lemon/utils/http_client.h>

using namespace lemon;
using namespace lemon::utils;

// Helper: Check if server is running
bool is_server_running(const std::string& host, int port) {
    std::string url = "http://" + host + ":" + std::to_string(port) + "/health";
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
            Server server(config.port, config.host, config.log_level,
                        config.ctx_size, config.tray, config.llamacpp_backend);
            server.run();
            
        } else if (command == "status") {
            // TODO: Implement status checking
            std::cout << "Status command not yet implemented" << std::endl;
            
        } else if (command == "stop") {
            // TODO: Implement server stopping
            std::cout << "Stop command not yet implemented" << std::endl;
            
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
            
            // Get models via API
            auto response = api_request("GET", "/api/v1/models");
            
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
                
                nlohmann::json request = {{"model", model_name}};
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

