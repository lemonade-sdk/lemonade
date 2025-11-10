#include "lemon/backends/vllm_server.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/error_types.h"
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>

namespace lemon {
namespace backends {

VllmServer::VllmServer(const std::string& log_level)
    : WrappedServer("vllm-server", log_level),
      docker_image_("rocm/vllm-dev:rocm7.1_navi_ubuntu24.04_py3.12_pytorch_2.8_vllm_0.10.2rc1") {
}

VllmServer::~VllmServer() {
    unload();
}

bool VllmServer::is_docker_available() {
    // Check if docker command is available
    int result = system("docker --version >/dev/null 2>&1");
    return result == 0;
}

bool VllmServer::is_docker_image_available(const std::string& image) {
    // Check if the Docker image exists locally
    std::string command = "docker images -q " + image + " 2>/dev/null";
    
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }
    
    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    
    // If we got an image ID back, the image exists
    return !result.empty();
}

void VllmServer::pull_docker_image(const std::string& image) {
    std::cout << "[VLLM] Pulling Docker image: " << image << std::endl;
    std::cout << "[VLLM] This may take several minutes..." << std::endl;
    
    std::string command = "docker pull " + image;
    int result = system(command.c_str());
    
    if (result != 0) {
        throw std::runtime_error("Failed to pull Docker image: " + image);
    }
    
    std::cout << "[VLLM] Docker image pulled successfully" << std::endl;
}

bool VllmServer::is_container_running(const std::string& container_name) {
    std::string command = "docker ps --filter name=" + container_name + " --filter status=running -q 2>/dev/null";
    
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }
    
    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    
    return !result.empty();
}

void VllmServer::stop_docker_container(const std::string& container_name) {
    std::cout << "[VLLM] Stopping Docker container: " << container_name << std::endl;
    
    // First try to stop the container gracefully
    std::string stop_command = "docker stop " + container_name + " >/dev/null 2>&1";
    int stop_result = system(stop_command.c_str());
    (void)stop_result;  // Intentionally ignore return value
    
    // Remove the container
    std::string rm_command = "docker rm -f " + container_name + " >/dev/null 2>&1";
    int rm_result = system(rm_command.c_str());
    (void)rm_result;  // Intentionally ignore return value
}

std::string VllmServer::start_docker_container(const std::string& model_checkpoint, int port) {
    std::string container_name = "lemonade-vllm-server";
    
    // Stop any existing container with the same name
    stop_docker_container(container_name);
    
    // Build the docker run command
    std::stringstream cmd;
    cmd << "docker run -d "
        << "--privileged "
        << "--device=/dev/kfd "
        << "--device=/dev/dri "
        << "--network=host "
        << "--group-add sudo "
        << "-w /app/vllm/ "
        << "--name " << container_name << " "
        << docker_image_ << " "
        << "vllm serve " << model_checkpoint << " --port " << port;
    
    std::string command = cmd.str();
    
    std::cout << "[VLLM] Starting Docker container..." << std::endl;
    if (is_debug()) {
        std::cout << "[VLLM] Command: " << command << std::endl;
    }
    
    // Execute the docker run command
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to start Docker container");
    }
    
    char buffer[128];
    std::string container_id = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        container_id += buffer;
    }
    int result = pclose(pipe);
    
    if (result != 0) {
        throw std::runtime_error("Failed to start Docker container (exit code: " + std::to_string(result) + ")");
    }
    
    // Trim whitespace from container ID
    container_id.erase(container_id.find_last_not_of(" \n\r\t") + 1);
    
    std::cout << "[VLLM] Docker container started: " << container_name << std::endl;
    
    return container_name;
}

void VllmServer::install(const std::string& backend) {
    std::cout << "[VLLM] Checking Docker installation..." << std::endl;
    
    if (!is_docker_available()) {
        throw std::runtime_error(
            "Docker is not installed or not in PATH. "
            "Please install Docker to use vllm backend: https://docs.docker.com/get-docker/"
        );
    }
    
    std::cout << "[VLLM] Docker is available" << std::endl;
    
    // Check if the Docker image is available
    if (!is_docker_image_available(docker_image_)) {
        std::cout << "[VLLM] Docker image not found locally, pulling..." << std::endl;
        pull_docker_image(docker_image_);
    } else {
        std::cout << "[VLLM] Docker image found: " << docker_image_ << std::endl;
    }
}

std::string VllmServer::download_model(const std::string& checkpoint,
                                      const std::string& mmproj,
                                      bool do_not_upgrade) {
    // For vllm, the model will be downloaded by the container on first run
    // We just return the checkpoint name
    std::cout << "[VLLM] Model " << checkpoint << " will be downloaded by vllm on first run" << std::endl;
    return checkpoint;
}

void VllmServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     int ctx_size,
                     bool do_not_upgrade) {
    std::cout << "[VLLM] Loading model: " << model_name << std::endl;
    
    // Store model checkpoint
    model_checkpoint_ = model_info.checkpoint;
    
    // Install/check Docker and image
    install();
    
    // Choose a port
    port_ = choose_port();
    
    // Start Docker container
    container_name_ = start_docker_container(model_checkpoint_, port_);
    
    // Wait for vllm server to be ready
    bool ready = wait_for_ready();
    if (!ready) {
        stop_docker_container(container_name_);
        throw std::runtime_error("vllm-server failed to start");
    }
    
    std::cout << "[VLLM] Model loaded on port " << port_ << std::endl;
}

void VllmServer::unload() {
    std::cout << "[VLLM] Unloading model..." << std::endl;
    if (!container_name_.empty()) {
        stop_docker_container(container_name_);
        container_name_.clear();
        port_ = 0;
        model_checkpoint_.clear();
    }
}

bool VllmServer::wait_for_ready() {
    // vllm uses /v1/models endpoint to check if it's ready
    std::string models_url = get_base_url() + "/v1/models";
    
    std::cout << "Waiting for " + server_name_ + " to be ready..." << std::endl;
    
    // Wait up to 10 minutes (vllm can take time to download and load models)
    const int max_attempts = 600;  // 10 minutes at 1 second intervals
    
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // Check if container is still running
        if (!is_container_running(container_name_)) {
            std::cerr << "[ERROR] " << server_name_ << " container has stopped!" << std::endl;
            std::cerr << "[ERROR] Check Docker logs with: docker logs " << container_name_ << std::endl;
            return false;
        }
        
        // Try to reach the /v1/models endpoint
        if (utils::HttpClient::is_reachable(models_url, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return true;
        }
        
        // Print progress every 30 seconds
        if (attempt % 30 == 0 && attempt > 0) {
            std::cout << "[VLLM] Still waiting... (" << attempt << "s elapsed)" << std::endl;
            std::cout << "[VLLM] vllm may be downloading the model on first run" << std::endl;
        }
        
        // Wait 1 second before next attempt
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cerr << "[ERROR] " << server_name_ << " failed to start within " 
              << max_attempts << " seconds" << std::endl;
    std::cerr << "[ERROR] Check Docker logs with: docker logs " << container_name_ << std::endl;
    return false;
}

json VllmServer::chat_completion(const json& request) {
    // vllm requires the correct model name in the request
    json modified_request = request;
    modified_request["model"] = model_checkpoint_;
    
    return forward_request("/v1/chat/completions", modified_request);
}

json VllmServer::completion(const json& request) {
    json modified_request = request;
    modified_request["model"] = model_checkpoint_;
    
    return forward_request("/v1/completions", modified_request);
}

json VllmServer::responses(const json& request) {
    // Responses API is not supported for vllm backend
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "vllm")
    );
}

void VllmServer::parse_telemetry(const std::string& line) {
    // vllm telemetry parsing can be added here if needed
    // For now, we'll rely on the response from the server
}

bool VllmServer::is_process_running() const {
    // For vllm, check if the Docker container is running instead of checking process status
    if (container_name_.empty()) {
        return false;
    }
    return const_cast<VllmServer*>(this)->is_container_running(container_name_);
}

} // namespace backends
} // namespace lemon

