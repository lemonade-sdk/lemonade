#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
#include <lemon/error_types.h>
#include <thread>
#include <chrono>
#include <iostream>

namespace lemon {

int WrappedServer::choose_port() {
    port_ = utils::ProcessManager::find_free_port(8001);
    if (port_ < 0) {
        throw std::runtime_error("Failed to find free port for " + server_name_);
    }
    std::cout << server_name_ << " will use port: " << port_ << std::endl;
    return port_;
}

bool WrappedServer::wait_for_ready() {
    // Try both /health and /v1/health (FLM uses /v1/health, llama-server uses /health)
    std::string health_url = get_base_url() + "/health";
    std::string health_url_v1 = get_base_url() + "/v1/health";
    
    std::cout << "Waiting for " + server_name_ + " to be ready..." << std::endl;
    
    // Wait up to 60 seconds for server to start
    for (int i = 0; i < 600; i++) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[ERROR] " << server_name_ << " process has terminated with exit code: " 
                     << exit_code << std::endl;
            std::cerr << "[ERROR] This usually means:" << std::endl;
            std::cerr << "  - Missing required drivers or dependencies" << std::endl;
            std::cerr << "  - Incompatible model file" << std::endl;
            std::cerr << "  - Try running the server manually to see the actual error" << std::endl;
            return false;
        }
        
        // Try both health endpoints
        if (utils::HttpClient::is_reachable(health_url, 1) || 
            utils::HttpClient::is_reachable(health_url_v1, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            std::cout << "Still waiting for " + server_name_ + "..." << std::endl;
        }
    }
    
    std::cerr << server_name_ + " failed to start within timeout" << std::endl;
    return false;
}

json WrappedServer::forward_request(const std::string& endpoint, const json& request) {
    if (!process_handle_.handle) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }
    
    std::string url = get_base_url() + endpoint;
    std::map<std::string, std::string> headers = {{"Content-Type", "application/json"}};
    
    try {
        auto response = utils::HttpClient::post(url, request.dump(), headers);
        
        if (response.status_code == 200) {
            return json::parse(response.body);
        } else {
            // Try to parse error response from backend
            json error_details;
            try {
                error_details = json::parse(response.body);
            } catch (...) {
                error_details = response.body;
            }
            
            return ErrorResponse::create(
                server_name_ + " request failed",
                ErrorType::BACKEND_ERROR,
                {
                    {"status_code", response.status_code},
                    {"response", error_details}
                }
            );
        }
    } catch (const std::exception& e) {
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

} // namespace lemon
