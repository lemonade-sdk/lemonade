#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
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
    std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    std::string health_url_v1 = "http://127.0.0.1:" + std::to_string(port_) + "/v1/health";
    
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

} // namespace lemon
