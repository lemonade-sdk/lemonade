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
#include "backends/backend_descriptor.h"

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
          state_(ModelState::LOADING),
          active_request_count_(0),
          maintenance_in_progress_(false),
          load_duration_ms_(0),
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

    // Pinned status for eviction prevention
    bool is_pinned() const { return pinned_; }
    void set_pinned(bool pinned) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pinned_ = pinned;
        if (pinned) recipe_options_.set_option("pinned", true);
        else recipe_options_.remove_option("pinned");
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

        // Wait out transient states: LOADING (initial load or an in-progress
        // restore) and an in-flight maintenance downsize. Waiting for the latter
        // ensures restore() below never races a concurrent downsize() on the same
        // backend subprocess. Looped because the state can change while we wait.
        while (state_ == ModelState::LOADING || maintenance_in_progress_) {
            state_cv_.wait(lock);
        }

        if (state_ == ModelState::UNLOADED) {
            return false;
        }

        if (state_ == ModelState::DOWNSIZED) {
            // Restore the model to full readiness before serving. (A downsize that
            // failed leaves the model READY, so only DOWNSIZED needs restoring.)
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

    void rescue_from_eviction() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == ModelState::EVICTING) {
            state_ = ModelState::READY;
            state_cv_.notify_all();
        }
    }

    void release_inference() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (--active_request_count_ == 0) {
            state_ = ModelState::READY;
            state_cv_.notify_all();
        }
    }

    // Called by the eviction engine (under the router lock) to atomically claim an
    // idle model for a maintenance downsize. Returns true only if the model is
    // currently READY and idle, transitioning it to DOWNSIZING and marking
    // maintenance in progress so wait_until_not_busy() — and therefore
    // evict_server() — blocks until the matching finish_downsize() runs. This is
    // what keeps the server alive while the engine performs downsize() outside the
    // router lock with only a raw pointer to it.
    bool try_begin_downsize() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == ModelState::READY && active_request_count_ == 0) {
            state_ = ModelState::DOWNSIZING;
            maintenance_in_progress_ = true;
            state_cv_.notify_all();
            return true;
        }
        return false;
    }

    // Completes the maintenance downsize started by try_begin_downsize(). Clears
    // the maintenance flag (releasing any waiters in wait_until_not_busy() /
    // acquire_for_inference()) and, while still DOWNSIZING, transitions to
    // DOWNSIZED on success or back to READY on failure so a failed backend
    // operation never leaves a model falsely marked as downsized.
    void finish_downsize(bool success) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        maintenance_in_progress_ = false;
        if (state_ == ModelState::DOWNSIZING) {
            state_ = success ? ModelState::DOWNSIZED : ModelState::READY;
        }
        state_cv_.notify_all();
    }

    bool is_busy() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return active_request_count_ > 0 || maintenance_in_progress_;
    }

    // Wait until the router no longer has active work using this object.
    // Returns true when the server is idle. Returns false if a bounded wait
    // timed out; callers must not destroy the WrappedServer in that case.
    bool wait_until_not_busy(int timeout_seconds = -1) const {
        std::unique_lock<std::mutex> lock(state_mutex_);
        auto not_busy = [this] {
            return active_request_count_ == 0 && !maintenance_in_progress_;
        };

        if (timeout_seconds < 0) {
            state_cv_.wait(lock, not_busy);
            return true;
        }

        return state_cv_.wait_for(
            lock,
            std::chrono::seconds(timeout_seconds),
            not_busy
        );
    }

    // Multi-model support: Model metadata
    void set_model_metadata(const std::string& model_name, const std::string& checkpoint,
                           ModelType type, DeviceType device, const RecipeOptions& recipe_options) {
        std::lock_guard<std::mutex> lock(state_mutex_);
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
    RecipeOptions get_recipe_options() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return recipe_options_;
    }
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

    void set_load_cancel_flag(std::atomic<bool>* f) { load_cancel_ = f; }

    static void set_request_cancel_flag(std::atomic<bool>* f);
    static std::atomic<bool>* current_request_cancel();

    // Downsize the model on soft idle (e.g., clear KV cache). Returns true if the
    // downsize succeeded (or was a no-op), false if the backend operation failed.
    // The default is a successful no-op: backends that cannot downsize transition
    // to DOWNSIZED once and are not retried while idle.
    virtual bool downsize() {
        // No-op by default
        return true;
    }

    // Restore the model from a downsized state
    virtual void restore() {
        // No-op by default
    }

    // Default to an "unsupported" error so non-chat backends (TTS, image,
    // transcription) inherit a sensible response instead of stubbing each one.
    virtual json chat_completion(const json& request) override {
        return unsupported_capability_error("chat completion");
    }
    virtual json completion(const json& request) override {
        return unsupported_capability_error("text completion");
    }
    virtual json responses(const json& request) {
        return unsupported_capability_error("responses");
    }

    // Descriptor association (set by the backend registry at create() time). The
    // effective_* hooks below default to the descriptor's declared values; a
    // backend whose device or eviction rule depends on the chosen backend
    // variant overrides them (e.g. whisper on npu vs cpu, llamacpp on cpu vs gpu).
    void set_descriptor(const BackendDescriptor* descriptor) { descriptor_ = descriptor; }
    const BackendDescriptor* get_descriptor() const { return descriptor_; }

    // Effective accelerator device for this load. The router calls this after it
    // resolves the "<recipe>_backend" option but before eviction. Defaults to the
    // descriptor's default_device; variant-dependent backends override.
    virtual DeviceType effective_device(const RecipeOptions& options) const {
        (void)options;
        return descriptor_ ? descriptor_->default_device : device_type_;
    }

    // Effective slot/eviction policy for this load. The router switches on this
    // value to enforce NPU exclusivity and LRU slot accounting. Defaults to the
    // descriptor's slot_policy; variant-dependent backends override.
    virtual SlotPolicy effective_slot_policy(const RecipeOptions& options) const {
        (void)options;
        return descriptor_ ? descriptor_->slot_policy : SlotPolicy::Standard;
    }

    // Dynamic availability check. Returns "" if the backend can run on this
    // system, or a user-facing reason why it cannot. Defaults to "available";
    // backends with runtime-dependent availability (cloud) override.
    virtual std::string availability() const { return ""; }

    // Forward streaming requests to the wrapped server (public for Router access)
    // Virtual so backends can transform request (e.g., FLM needs checkpoint in model field)
    using TelemetryCallback = std::function<void(int input_tokens,
                                                 int output_tokens,
                                                 double time_to_first_token,
                                                 double tokens_per_second,
                                                 const std::string& error_message)>;

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

    virtual std::map<std::string, nlohmann::json> get_additional_telemetry() {
        return {};
    }

    virtual std::string get_additional_telemetry_url() const {
        return "";
    }

    virtual std::function<std::map<std::string, nlohmann::json>(const std::string&)> get_additional_telemetry_parser() const {
        return nullptr;
    }

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

    // Standard "this backend does not serve <what>" error payload, matching the
    // shape backends return from unsupported capability methods.
    json unsupported_capability_error(const std::string& what) const {
        return json{{"error", {
            {"message", server_name_ + " does not support " + what +
                            ". Use the appropriate endpoint for this model type instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}};
    }

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
    const BackendDescriptor* descriptor_ = nullptr;  // Non-owning; set by the backend registry at create()

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

    // True while the eviction engine is performing a maintenance downsize on this
    // server. Counts as "busy" so wait_until_not_busy() (and therefore
    // evict_server()) blocks until the operation completes, preventing the server
    // from being unloaded/destroyed while the engine holds a raw pointer to it.
    bool maintenance_in_progress_;
    long load_duration_ms_;
    bool pinned_ = false;
    std::atomic<bool>* load_cancel_ = nullptr;

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
