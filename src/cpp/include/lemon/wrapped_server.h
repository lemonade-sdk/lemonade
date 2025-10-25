#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "utils/process_manager.h"
#include "server_capabilities.h"

namespace lemon {

using json = nlohmann::json;
using utils::ProcessHandle;

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

class WrappedServer : public ICompletionServer {
public:
    WrappedServer(const std::string& server_name, const std::string& log_level = "info")
        : server_name_(server_name), port_(0), process_handle_({nullptr, 0}), log_level_(log_level) {}
    
    virtual ~WrappedServer() = default;
    
    // Set log level
    void set_log_level(const std::string& log_level) { log_level_ = log_level; }
    
    // Check if debug logging is enabled
    bool is_debug() const { return log_level_ == "debug" || log_level_ == "trace"; }
    
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
                     bool do_not_upgrade = false,
                     const std::vector<std::string>& labels = {}) = 0;
    
    // Unload the model and stop the server
    virtual void unload() = 0;
    
    // ICompletionServer implementation - forward requests to the wrapped server
    virtual json chat_completion(const json& request) override = 0;
    virtual json completion(const json& request) override = 0;
    virtual json responses(const json& request) = 0;
    
    // Get the server address
    std::string get_address() const {
        return get_base_url() + "/v1";
    }
    
    // Get telemetry data
    Telemetry get_telemetry() const { return telemetry_; }
    
protected:
    // Choose an available port
    int choose_port();
    
    // Wait for server to be ready (can be overridden for custom health checks)
    virtual bool wait_for_ready();
    
    // Parse telemetry from subprocess output
    virtual void parse_telemetry(const std::string& line) = 0;
    
    // Common method to forward requests to the wrapped server
    json forward_request(const std::string& endpoint, const json& request);
    
    // Get the base URL for the wrapped server
    std::string get_base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    
    std::string server_name_;
    int port_;
    ProcessHandle process_handle_;
    Telemetry telemetry_;
    std::string log_level_;
};

} // namespace lemon

