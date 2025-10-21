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
    std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    
    std::cout << "Waiting for " + server_name_ + " to be ready on " << health_url << "..." << std::endl;
    
    // Wait up to 60 seconds for server to start
    for (int i = 0; i < 600; i++) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[ERROR] " << server_name_ << " process has terminated with exit code: " 
                     << exit_code << std::endl;
            std::cerr << "[ERROR] This usually means:" << std::endl;
            std::cerr << "  - Missing Vulkan drivers (install GPU drivers)" << std::endl;
            std::cerr << "  - Missing DLL dependencies" << std::endl;
            std::cerr << "  - Incompatible model file" << std::endl;
            std::cerr << "  - Run the llama-server.exe manually to see the actual error" << std::endl;
            return false;
        }
        
        if (utils::HttpClient::is_reachable(health_url, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print progress every 5 seconds
        if (i % 50 == 0 && i > 0) {
            std::cout << "Still waiting for " + server_name_ + "... (checking " << health_url << ")" << std::endl;
        }
    }
    
    std::cerr << server_name_ + " failed to start within timeout" << std::endl;
    return false;
}

} // namespace lemon
