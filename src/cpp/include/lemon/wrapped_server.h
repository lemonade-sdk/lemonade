#pragma once

#include <string>
#include <memory>
#include <functional>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <stdexcept>
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

class BackendStreamRetryableReset : public std::runtime_error {
public:
    explicit BackendStreamRetryableReset(const std::string& reason)
        : std::runtime_error(reason) {}
};


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
          is_busy_(false),
          busy_count_(0),
          last_backend_activity_(std::chrono::steady_clock::now()) {}

    virtual ~WrappedServer();


    void set_log_level(const std::string& log_level) { log_level_ = log_level; }

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
        if (busy) {
            ++busy_count_;
        } else if (busy_count_ > 0) {
            --busy_count_;
        }
        is_busy_ = busy_count_ > 0;
        if (!is_busy_) {
            busy_cv_.notify_all();
        }
    }

    bool is_busy() const {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        return is_busy_;
    }

    // Wait until the router no longer has active work using this object.
    // Returns true when the server is idle. Returns false if a bounded wait
    // timed out; callers must not destroy the WrappedServer in that case.
    bool wait_until_not_busy(int timeout_seconds = -1) const {
        std::unique_lock<std::mutex> lock(busy_mutex_);
        if (timeout_seconds < 0) {
            busy_cv_.wait(lock, [this] { return !is_busy_; });
            return true;
        }
        return busy_cv_.wait_for(lock, std::chrono::seconds(timeout_seconds),
                                 [this] { return !is_busy_; });
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
    int get_process_id() const { return get_process_handle_snapshot().pid; }
    int get_backend_port() const;

    // Cheap liveness gate used by the router. On POSIX this relies on
    // ProcessManager::is_running(), which intentionally checks without reaping.
    virtual bool is_backend_alive() const;

    // True once the backend watchdog force-reset the child process.
    bool was_watchdog_triggered() const { return watchdog_triggered_.load(std::memory_order_acquire); }

    // Human-readable state for /health and debugging endpoints.
    virtual std::string get_backend_health_state() const;
    std::string get_watchdog_reset_reason() const;

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

    Telemetry get_telemetry() const { return telemetry_; }

    // Mark observable backend progress. Streaming proxies call this for every
    // delivered chunk; non-streaming requests call it on start/finish and when
    // the watchdog observes a healthy out-of-band probe.
    void note_backend_activity();

    void set_telemetry(int input_tokens, int output_tokens,
                      double time_to_first_token, double tokens_per_second) {
        telemetry_.input_tokens = input_tokens;
        telemetry_.output_tokens = output_tokens;
        telemetry_.time_to_first_token = time_to_first_token;
        telemetry_.tokens_per_second = tokens_per_second;
    }

    void set_prompt_tokens(int prompt_tokens) {
        telemetry_.prompt_tokens = prompt_tokens;
    }

protected:
    struct BackendWatchdogPolicy {
        std::string health_endpoint = "/health";
        bool enabled = true;
        bool monitor_streaming_requests = true;
    };

    enum class BackendRequestKind {
        NonStreaming,
        Streaming
    };

    class BackendRequestScope {
    public:
        BackendRequestScope(WrappedServer& server, BackendRequestKind kind);
        ~BackendRequestScope();
        BackendRequestScope(const BackendRequestScope&) = delete;
        BackendRequestScope& operator=(const BackendRequestScope&) = delete;
    private:
        WrappedServer& server_;
        BackendRequestKind kind_;
    };

    static bool has_process_handle(const ProcessHandle& handle);
    ProcessHandle get_process_handle_snapshot() const;
    void set_process_handle(ProcessHandle handle);
    ProcessHandle consume_process_handle_for_cleanup();

    // Choose an available port
    int choose_port();

    // Wait for server to be ready (can be overridden for custom health checks)
    virtual bool wait_for_ready(const std::string& endpoint, long timeout_seconds = 600, long poll_interval_ms = 100);

    // Configure/start the generic backend watchdog. Non-streaming requests are
    // always monitored so a hung backend becomes a reload+retry delay instead
    // of a stuck user request. Streaming can still avoid replaying partial data.
    void configure_backend_watchdog(const BackendWatchdogPolicy& policy);
    void start_backend_watchdog(const std::string& health_endpoint);
    void start_backend_watchdog(const BackendWatchdogPolicy& policy);
    void stop_backend_watchdog();
    void set_watchdog_health_endpoint(const std::string& endpoint);

    // Common method to forward requests to the wrapped server (non-streaming)
    json forward_request(const std::string& endpoint, const json& request, long timeout_seconds = 0);

    json forward_get_request(const std::string& endpoint, long timeout_seconds = 0);

    // Forward multipart form data to the wrapped server
    json forward_multipart_request(const std::string& endpoint,
                                   const std::vector<utils::MultipartField>& fields,
                                   long timeout_seconds = 0);

    // Validate that the process is running (platform-agnostic check)
    bool is_process_running() const;

    std::string get_base_url() const {
        return "http://127.0.0.1:" + std::to_string(get_backend_port());
    }

    json create_watchdog_reset_response() const;

    std::string server_name_;
    int port_;
    ProcessHandle process_handle_;
    mutable std::mutex process_mutex_;
    Telemetry telemetry_;
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
    mutable std::mutex busy_mutex_;
    mutable std::condition_variable busy_cv_;
    bool is_busy_;
    int busy_count_;

private:
    void begin_backend_request(BackendRequestKind kind);
    void end_backend_request(BackendRequestKind kind);
    void backend_watchdog_loop();
    bool has_backend_process_exited() const;
    void request_backend_reset_from_watchdog(const std::string& reason);

    mutable std::mutex watchdog_mutex_;
    std::condition_variable watchdog_cv_;
    std::thread watchdog_thread_;
    BackendWatchdogPolicy watchdog_policy_;
    std::chrono::steady_clock::time_point last_backend_activity_;
    std::string watchdog_reset_reason_;
    std::atomic<bool> watchdog_stop_requested_{false};
    std::atomic<bool> watchdog_running_{false};
    std::atomic<bool> watchdog_triggered_{false};
    std::atomic<int> active_backend_requests_{0};
    std::atomic<int> active_streaming_requests_{0};
    std::atomic<int> active_non_streaming_requests_{0};
};

} // namespace lemon
