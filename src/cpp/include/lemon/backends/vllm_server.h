#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class VllmServer : public WrappedServer {
public:
    VllmServer(const std::string& log_level = "info");
    
    ~VllmServer() override;
    
    void install(const std::string& backend = "") override;
    
    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;
    
    void load(const std::string& model_name,
             const ModelInfo& model_info,
             int ctx_size,
             bool do_not_upgrade = false) override;
    
    void unload() override;
    
    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    
protected:
    void parse_telemetry(const std::string& line) override;
    
private:
    // Docker container management
    bool is_docker_available();
    bool is_docker_image_available(const std::string& image);
    void pull_docker_image(const std::string& image);
    std::string start_docker_container(const std::string& model_checkpoint, int port);
    void stop_docker_container(const std::string& container_name);
    bool is_container_running(const std::string& container_name);
    
    // Check if vllm server is ready
    bool wait_for_ready() override;
    
    std::string container_name_;
    std::string docker_image_;
    std::string model_checkpoint_;
};

} // namespace backends
} // namespace lemon

