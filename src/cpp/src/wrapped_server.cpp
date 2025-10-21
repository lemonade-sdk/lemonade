#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
#include <thread>
#include <chrono>
#include <iostream>

namespace lemon {

void WrappedServer::choose_port() {
    port_ = utils::ProcessManager::find_free_port(8001);
    if (port_ < 0) {
        throw std::runtime_error("Failed to find free port for " + server_name_);
    }
    std::cout << server_name_ << " will use port: " << port_ << std::endl;
}

void WrappedServer::wait_for_ready() {
    std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    
    std::cout << "Waiting for " + server_name_ + " to be ready..." << std::endl;
    
    // Wait up to 60 seconds for server to start
    for (int i = 0; i < 600; i++) {
        if (utils::HttpClient::is_reachable(health_url, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            std::cout << "Still waiting for " + server_name_ + "..." << std::endl;
        }
    }
    
    throw std::runtime_error(server_name_ + " failed to start within timeout");
}

} // namespace lemon
