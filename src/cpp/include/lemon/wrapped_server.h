#pragma once

#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "utils/process_manager.h"
#include "utils/http_client.h"
#include "server_capabilities.h"
#include "model_manager.h"
#include "backend_manager.h"
#include "recipe_options.h"

namespace lemon {

using json = nlohmann::json;
using utils::ProcessHandle;

struct Telemetry {
    int input_tokens = 0;
    int output_tokens = 0;
    double time_to_first_token = 0.0;
    double tokens_per_second = 0.0;
    int prompt_tokens = 0;  // From usage.prompt_tokens (includes cached tokens)
    uint64_t request_count_total = 0;
    uint64_t input_tokens_total = 0;
    uint64_t output_tokens_total = 0;
    uint64_t prompt_tokens_total = 0;

    void reset() {
        input_tokens = 0;
        output_tokens = 0;
        time_to_first_token = 0.0;
        tokens_per_second = 0.0;
        prompt_tokens = 0;
        request_count_total = 0;
        input_tokens_total = 0;
        output_tokens_total = 0;
        prompt_tokens_total = 0;
    }

    json to_json() const {
        return {
            {"input_tokens", input_tokens},
            {"output_tokens", output_tokens},
            {"time_to_first_token", time_to_first_token},
            {"tokens_per_second", tokens_per_second},
            {"prompt_tokens", prompt_tokens},
            {"request_count_total", request_count_total},
            {"input_tokens_total", input_tokens_total},
            {"output_tokens_total", output_tokens_total},
            {"prompt_tokens_total", prompt_tokens_total}
        };
    }
};

class WrappedServer : public ICompletionServer {
public:
    WrappedServer(const std::string& server_name, const std::string& log_level,
                  ModelManager* model_manager = nullptr, BackendManager* backend_manager = nullptr)
        : server_name_(server_name), port_(0), process_handle_({nullptr, 0}), log_level_(log_level),
          model_manager_(model_manager), backend_manager_(backend_manager),
          last_access_time_(std::chrono::steady_clock::now()),
          state_(ModelState::LOADING),
          active_request_count_(0),
          load_duration_ms_(0) {}

    virtual ~WrappedServer() = default;


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

    // State management
    ModelState get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

    void set_state(ModelState new_state) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = new_state;
        state_cv_.notify_all();
    }

    void set_load_duration_ms(long ms) {
        load_duration_ms_ = ms;
    }

    long get_load_duration_ms() const {
        return load_duration_ms_;
    }

    // Acquire model for inference, safely recovering from DOWNSIZING/EVICTING if necessary.
    // Blocks if LOADING.
    //
    // Concurrency contract with the eviction engine (see try_commit_eviction):
    //   - EVICTING is *tentative*. The engine marks the model EVICTING under
    //     state_mutex_, then later calls try_commit_eviction() — also under
    //     state_mutex_ — to atomically decide whether to physically unload.
    //   - If a request arrives while still EVICTING (pre-commit), we "rescue" the
    //     model here: flip back to IN_USE so try_commit_eviction() sees it is no
    //     longer evictable and aborts the unload. No reload, no torn state.
    //   - Once the engine commits, it sets UNLOADED before releasing state_mutex_,
    //     so any later acquire observes UNLOADED and returns false (router reloads).
    // Because both paths take state_mutex_, the rescue/commit decision is atomic.
    bool acquire_for_inference() {
        std::unique_lock<std::mutex> lock(state_mutex_);

        while (state_ == ModelState::LOADING) {
            state_cv_.wait(lock);
        }

        if (state_ == ModelState::UNLOADED) {
            return false;
        }

        if (state_ == ModelState::DOWNSIZING || state_ == ModelState::DOWNSIZED) {
            // Interrupt downsize and restore the model to full readiness.
            state_ = ModelState::LOADING; // temporarily block others
            lock.unlock();

            this->restore();

            lock.lock();
            state_ = ModelState::READY;
            state_cv_.notify_all();
        }

        // Covers READY, IN_USE, and EVICTING (rescue): claim the model.
        active_request_count_++;
        state_ = ModelState::IN_USE;
        state_cv_.notify_all();
        return true;
    }

    // Called by the eviction engine (under the router lock) to atomically decide
    // whether a model marked EVICTING may actually be unloaded. Returns true only
    // if the model is still idle and EVICTING (commit -> transition to UNLOADED so
    // later acquires reload). Returns false if a request rescued it (state changed
    // to IN_USE) or it is otherwise busy, reverting it to READY.
    bool try_commit_eviction() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == ModelState::EVICTING && active_request_count_ == 0) {
            state_ = ModelState::UNLOADED;
            state_cv_.notify_all();
            return true;
        }
        // Rescued or busy: abandon the eviction.
        if (state_ == ModelState::EVICTING) {
            state_ = ModelState::READY;
            state_cv_.notify_all();
        }
        return false;
    }

    void release_inference() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (--active_request_count_ == 0) {
            state_ = ModelState::READY;
            state_cv_.notify_all();
        }
    }

    bool is_busy() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return active_request_count_ > 0;
    }

    // Wait until the server is no longer busy processing a request.
    void wait_until_not_busy(int timeout_seconds = -1) const {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (timeout_seconds < 0) {
            while (active_request_count_ > 0) {
                state_cv_.wait(lock);
            }
        } else {
            if (!state_cv_.wait_for(lock, std::chrono::seconds(timeout_seconds),
                                   [this] { return active_request_count_ == 0; })) {
                // Timeout expired
            }
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
    int get_process_id() const { return process_handle_.pid; }

    // Load a model and start the server
    virtual void load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade = false) = 0;

    // Unload the model and stop the server
    virtual void unload() = 0;

    // Downsize the model on soft idle (e.g., clear KV cache)
    virtual void downsize() {
        // No-op by default
    }

    // Restore the model from a downsized state
    virtual void restore() {
        // No-op by default
    }

    // ICompletionServer implementation - forward requests to the wrapped server
    virtual json chat_completion(const json& request) override = 0;
    virtual json completion(const json& request) override = 0;
    virtual json responses(const json& request) = 0;

    // Forward streaming requests to the wrapped server (public for Router access)
    // Virtual so backends can transform request (e.g., FLM needs checkpoint in model field)
    using TelemetryCallback = std::function<void(int input_tokens,
                                                int output_tokens,
                                                double time_to_first_token,
                                                double tokens_per_second)>;

    virtual void forward_streaming_request(const std::string& endpoint,
                                           const std::string& request_body,
                                           httplib::DataSink& sink,
                                           bool sse = true,
                                           long timeout_seconds = 0,
                                           TelemetryCallback telemetry_callback = nullptr);

    // Get the server address
    std::string get_address() const {
        return get_base_url() + "/v1";
    }

protected:
    // Choose an available port
    int choose_port();

    // Wait for server to be ready (can be overridden for custom health checks)
    virtual bool wait_for_ready(const std::string& endpoint, long timeout_seconds = 600, long poll_interval_ms = 100);

    // Common method to forward requests to the wrapped server (non-streaming)
    json forward_request(const std::string& endpoint, const json& request, long timeout_seconds = 0);

    // Forward multipart form data to the wrapped server
    json forward_multipart_request(const std::string& endpoint,
                                   const std::vector<utils::MultipartField>& fields,
                                   long timeout_seconds = 0);

    // Validate that the process is running (platform-agnostic check)
    bool is_process_running() const;

    // Get the base URL for the wrapped server
    std::string get_base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::string server_name_;
    int port_;
    ProcessHandle process_handle_;
    std::string log_level_;
    ModelManager* model_manager_;  // Non-owning pointer to ModelManager
    BackendManager* backend_manager_;  // Non-owning pointer to BackendManager

    // Multi-model support fields
    std::string model_name_;
    std::string checkpoint_;
    ModelType model_type_ = ModelType::LLM;
    DeviceType device_type_ = DEVICE_NONE;
    std::chrono::steady_clock::time_point last_access_time_;
    RecipeOptions recipe_options_;

    // Busy state tracking (for safe eviction)
    mutable std::mutex state_mutex_;
    mutable std::condition_variable state_cv_;
    ModelState state_;
    int active_request_count_;
    long load_duration_ms_;
};

} // namespace lemon
