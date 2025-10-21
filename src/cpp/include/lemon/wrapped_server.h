#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

struct Telemetry {
    int input_tokens = 0;
    int output_tokens = 0;
    double time_to_first_token = 0.0;
    double tokens_per_second = 0.0;
    std::vector<double> decode_token_times;
    
    void reset() {
        input_tokens = 0;
        output_tokens = 0;
        time_to_first_token = 0.0;
        tokens_per_second = 0.0;
        decode_token_times.clear();
    }
    
    json to_json() const {
        return {
            {"input_tokens", input_tokens},
            {"output_tokens", output_tokens},
            {"time_to_first_token", time_to_first_token},
            {"tokens_per_second", tokens_per_second},
            {"decode_token_times", decode_token_times}
        };
    }
};

class WrappedServer {
public:
    WrappedServer(const std::string& server_name)
        : server_name_(server_name), port_(0), process_handle_(nullptr) {}
    
    virtual ~WrappedServer() = default;
    
    // Install the backend server
    virtual void install(const std::string& backend = "") = 0;
    
    // Download model files
    virtual std::string download_model(const std::string& checkpoint,
                                      const std::string& mmproj = "",
                                      bool do_not_upgrade = false) = 0;
    
    // Load a model and start the server
    virtual void load(const std::string& model_name,
                     const std::string& checkpoint,
                     const std::string& mmproj,
                     int ctx_size,
                     bool do_not_upgrade = false) = 0;
    
    // Unload the model and stop the server
    virtual void unload() = 0;
    
    // Forward requests to the wrapped server
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;
    virtual json embeddings(const json& request) = 0;
    virtual json reranking(const json& request) = 0;
    
    // Get the server address
    std::string get_address() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    
    // Get telemetry data
    Telemetry get_telemetry() const { return telemetry_; }
    
protected:
    // Choose an available port
    void choose_port();
    
    // Wait for server to be ready
    void wait_for_ready();
    
    // Parse telemetry from subprocess output
    virtual void parse_telemetry(const std::string& line) = 0;
    
    std::string server_name_;
    int port_;
    void* process_handle_;  // Platform-specific process handle
    Telemetry telemetry_;
};

} // namespace lemon

