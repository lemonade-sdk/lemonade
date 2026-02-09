#pragma once

#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "utils/process_manager.h"
#include "server_capabilities.h"
#include "model_manager.h"
#include "recipe_options.h"

namespace lemon {

using json = nlohmann::json;
using utils::ProcessHandle;

struct Telemetry {
    int input_tokens = 0;
    int output_tokens = 0;
    double time_to_first_token = 0.0;
    double tokens_per_second = 0.0;
    std::vector<double> decode_token_times;
    int prompt_tokens = 0;  // From usage.prompt_tokens (includes cached tokens)

    void reset() {
        input_tokens = 0;
        output_tokens = 0;
        time_to_first_token = 0.0;
        tokens_per_second = 0.0;
        decode_token_times.clear();
        prompt_tokens = 0;
    }

    json to_json() const {
        return {
            {"input_tokens", input_tokens},
            {"output_tokens", output_tokens},
            {"time_to_first_token", time_to_first_token},
            {"tokens_per_second", tokens_per_second},
            {"decode_token_times", decode_token_times},
            {"prompt_tokens", prompt_tokens}
        };
    }
};

class WrappedServer : public ICompletionServer {
public:
    WrappedServer(const std::string& server_name, const std::string& log_level = "info", ModelManager* model_manager = nullptr)
        : server_name_(server_name), port_(0), process_handle_({nullptr, 0}), log_level_(log_level),
          model_manager_(model_manager), last_access_time_(std::chrono::steady_clock::now()),
          is_busy_(false) {}

    virtual ~WrappedServer() = default;

    // Timeout for inference requests (0 = infinite)
    static constexpr long INFERENCE_TIMEOUT_SECONDS = 0;

    // Set log level
    void set_log_level(const std::string& log_level) { log_level_ = log_level; }

    // Check if debug logging is enabled
    bool is_debug() const { return log_level_ == "debug" || log_level_ == "trace"; }

    // Multi-model support: Track last access time (for LRU eviction)
    void update_access_time() {
        last_access_time_ = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point get_last_access_time() const {
        return last_access_time_;
    }

    // Multi-model support: Track if server is currently processing a request
    void set_busy(bool busy) {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        is_busy_ = busy;
        if (!busy) {
            busy_cv_.notify_all();
        }
    }

    bool is_busy() const {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        return is_busy_;
    }

    void wait_until_not_busy() const {
        std::unique_lock<std::mutex> lock(busy_mutex_);
        while (is_busy_) {
            busy_cv_.wait(lock);
        }
    }

    // Multi-model support: Model metadata
    void set_model_metadata(const std::string& model_name, const std::string& checkpoint,
                           ModelType type, DeviceType device, const RecipeOptions& recipe_options) {
        model_name_ = model_name;
        checkpoint_ = checkpoint;
        model_type_ = type;
        device_type_ = device;
        recipe_options_ = recipe_options;
    }

    std::string get_model_name() const { return model_name_; }
    std::string get_checkpoint() const { return checkpoint_; }
    ModelType get_model_type() const { return model_type_; }
    DeviceType get_device_type() const { return device_type_; }
    RecipeOptions get_recipe_options() const { return recipe_options_; }

    // Install the backend server
    virtual void install(const std::string& backend = "") = 0;

    // Download model files
    virtual std::string download_model(const std::string& checkpoint,
                                      const std::string& mmproj = "",
                                      bool do_not_upgrade = false) = 0;

    // Load a model and start the server
    virtual void load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade = false) = 0;

    // Unload the model and stop the server
    virtual void unload() = 0;

    // ICompletionServer implementation - forward requests to the wrapped server
    virtual json chat_completion(const json& request) override = 0;
    virtual json completion(const json& request) override = 0;
    virtual json responses(const json& request) = 0;

    // Forward streaming requests to the wrapped server (public for Router access)
    // Virtual so backends can transform request (e.g., FLM needs checkpoint in model field)
    virtual void forward_streaming_request(const std::string& endpoint,
                                           const std::string& request_body,
                                           httplib::DataSink& sink,
                                           bool sse = true);

    // Get the server address
    std::string get_address() const {
        return get_base_url() + "/v1";
    }

    // Get telemetry data
    Telemetry get_telemetry() const { return telemetry_; }

    // Set telemetry data (for non-streaming requests)
    void set_telemetry(int input_tokens, int output_tokens,
                      double time_to_first_token, double tokens_per_second) {
        telemetry_.input_tokens = input_tokens;
        telemetry_.output_tokens = output_tokens;
        telemetry_.time_to_first_token = time_to_first_token;
        telemetry_.tokens_per_second = tokens_per_second;
    }

    // Set prompt_tokens field from usage
    void set_prompt_tokens(int prompt_tokens) {
        telemetry_.prompt_tokens = prompt_tokens;
    }

protected:
    // Choose an available port
    int choose_port();

    // Wait for server to be ready (can be overridden for custom health checks)
    virtual bool wait_for_ready(const std::string& endpoint, long timeout_seconds = 600, long poll_interval_ms = 100);

    // Common method to forward requests to the wrapped server (non-streaming)
    json forward_request(const std::string& endpoint, const json& request, long timeout_seconds = INFERENCE_TIMEOUT_SECONDS);

    // Validate that the process is running (platform-agnostic check)
    bool is_process_running() const;

    // Get the base URL for the wrapped server
    std::string get_base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::string server_name_;
    int port_;
    ProcessHandle process_handle_;
    Telemetry telemetry_;
    std::string log_level_;
    ModelManager* model_manager_;  // Non-owning pointer to ModelManager

    // Multi-model support fields
    std::string model_name_;
    std::string checkpoint_;
    ModelType model_type_ = ModelType::LLM;
    DeviceType device_type_ = DEVICE_NONE;
    std::chrono::steady_clock::time_point last_access_time_;
    RecipeOptions recipe_options_;

    // Busy state tracking (for safe eviction)
    mutable std::mutex busy_mutex_;
    mutable std::condition_variable busy_cv_;
    bool is_busy_;
};

} // namespace lemon
