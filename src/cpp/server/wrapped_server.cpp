#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
#include <lemon/streaming_proxy.h>
#include <lemon/error_types.h>
#include <httplib.h>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

static thread_local std::atomic<bool>* t_request_cancel = nullptr;

void WrappedServer::set_request_cancel_flag(std::atomic<bool>* f) { t_request_cancel = f; }

std::atomic<bool>* WrappedServer::current_request_cancel() { return t_request_cancel; }

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

long get_env_long(const char* name, long fallback, long min_value) {
    const char* raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return fallback;
    }

    try {
        long value = std::stol(raw);
        return value < min_value ? min_value : value;
    } catch (...) {
        return fallback;
    }
}

bool get_env_bool(const char* name, bool fallback) {
    const char* raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return fallback;
    }

    const std::string value = lower_copy(raw);
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    return fallback;
}

bool backend_watchdog_enabled() {
    return get_env_bool("LEMONADE_BACKEND_WATCHDOG", true);
}

std::string normalize_endpoint(std::string endpoint) {
    if (endpoint.empty()) {
        return endpoint;
    }
    if (endpoint[0] != '/') {
        endpoint.insert(endpoint.begin(), '/');
    }
    return endpoint;
}

bool is_backend_connection_failure(const std::string& message) {
    const std::string lowered = lower_copy(message);
    return lowered.find("server returned nothing") != std::string::npos ||
           lowered.find("empty reply") != std::string::npos ||
           lowered.find("failed to connect") != std::string::npos ||
           lowered.find("couldn't connect") != std::string::npos ||
           lowered.find("could not connect") != std::string::npos ||
           lowered.find("connection refused") != std::string::npos ||
           lowered.find("connection reset") != std::string::npos ||
           lowered.find("failure when receiving data") != std::string::npos ||
           lowered.find("transfer closed") != std::string::npos ||
           lowered.find("partial file") != std::string::npos ||
           lowered.find("stream before done") != std::string::npos;
}

bool is_context_window_error(const std::string& message) {
    const std::string lowered = lower_copy(message);
    return lowered.find("exceeds the available context size") != std::string::npos ||
           lowered.find("context length exceeded") != std::string::npos;
}

std::string extract_backend_error_message(const json& backend_response) {
    if (backend_response.is_object() && backend_response.contains("error")) {
        const auto& error = backend_response["error"];
        if (error.is_string()) {
            return error.get<std::string>();
        }
        if (error.is_object() && error.contains("message") && error["message"].is_string()) {
            return error["message"].get<std::string>();
        }
        return error.dump();
    }
    if (backend_response.is_string()) {
        return backend_response.get<std::string>();
    }
    return backend_response.dump();
}

json create_backend_error_response(const std::string& server_name, int status_code,
                                   const json& backend_response) {
    std::string message = extract_backend_error_message(backend_response);
    if (message.empty()) {
        message = server_name + " request failed";
    }

    json error = {
        {"message", message},
        {"type", status_code >= 400 && status_code < 500
            ? "invalid_request_error"
            : ErrorType::BACKEND_ERROR},
        {"status_code", status_code},
        {"details", {
            {"backend", server_name},
            {"response", backend_response}
        }}
    };

    if (backend_response.is_object() && backend_response.contains("error") &&
        backend_response["error"].is_object()) {
        const auto& backend_error = backend_response["error"];
        if (backend_error.contains("type") && backend_error["type"].is_string()) {
            error["type"] = backend_error["type"];
        }
        if (backend_error.contains("code")) {
            error["code"] = backend_error["code"];
        }
    }

    if (is_context_window_error(message)) {
        error["type"] = "invalid_request_error";
        error["code"] = "context_length_exceeded";
    }

    return {{"error", error}};
}

} // namespace

WrappedServer::~WrappedServer() {
    stop_backend_watchdog();
}

WrappedServer::BackendRequestScope::BackendRequestScope(WrappedServer& server, BackendRequestKind kind)
    : server_(server), kind_(kind) {
    server_.begin_backend_request(kind_);
}

WrappedServer::BackendRequestScope::~BackendRequestScope() {
    server_.end_backend_request(kind_);
}

bool WrappedServer::has_process_handle(const ProcessHandle& handle) {
#ifdef _WIN32
    return handle.handle != nullptr;
#else
    return handle.pid > 0;
#endif
}

ProcessHandle WrappedServer::get_process_handle_snapshot() const {
    std::lock_guard<std::mutex> lock(process_mutex_);
    return process_handle_;
}

void WrappedServer::set_process_handle(ProcessHandle handle) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    process_handle_ = handle;
}

int WrappedServer::get_backend_port() const {
    std::lock_guard<std::mutex> lock(process_mutex_);
    return port_;
}

ProcessHandle WrappedServer::consume_process_handle_for_cleanup() {
    std::lock_guard<std::mutex> lock(process_mutex_);
    ProcessHandle handle = process_handle_;
    process_handle_ = {nullptr, 0};
    port_ = 0;
    return handle;
}

bool WrappedServer::is_backend_alive() const {
    if (watchdog_triggered_.load(std::memory_order_acquire)) {
        return false;
    }
    const ProcessHandle handle = get_process_handle_snapshot();
    return has_process_handle(handle) && utils::ProcessManager::is_running(handle);
}

std::string WrappedServer::get_backend_health_state() const {
    if (watchdog_triggered_.load(std::memory_order_acquire)) {
        return "watchdog_reset";
    }
    const ProcessHandle handle = get_process_handle_snapshot();
    if (!has_process_handle(handle)) {
        return "stopped";
    }
    if (!utils::ProcessManager::is_running(handle)) {
        return "exited";
    }
    if (active_backend_requests_.load(std::memory_order_acquire) > 0) {
        return "busy";
    }
    return "ready";
}

std::string WrappedServer::get_watchdog_reset_reason() const {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    return watchdog_reset_reason_;
}

json WrappedServer::create_watchdog_reset_response() const {
    const std::string reason = get_watchdog_reset_reason();
    json details = {
        {"backend", server_name_},
        {"code", "backend_watchdog_reset"},
        {"retryable", true}
    };
    if (!reason.empty()) {
        details["reason"] = reason;
    }

    return ErrorResponse::create(
        server_name_ + " was unresponsive and has been reset by the backend watchdog. The model will be reloaded automatically on retry.",
        ErrorType::BACKEND_ERROR,
        details
    );
}

void WrappedServer::note_backend_activity() {
    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        last_backend_activity_ = std::chrono::steady_clock::now();
    }
    watchdog_cv_.notify_all();
}

void WrappedServer::begin_backend_request(BackendRequestKind kind) {
    active_backend_requests_.fetch_add(1, std::memory_order_acq_rel);
    if (kind == BackendRequestKind::Streaming) {
        active_streaming_requests_.fetch_add(1, std::memory_order_acq_rel);
    } else {
        active_non_streaming_requests_.fetch_add(1, std::memory_order_acq_rel);
    }
    note_backend_activity();
}

void WrappedServer::end_backend_request(BackendRequestKind kind) {
    note_backend_activity();

    int previous = active_backend_requests_.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 1) {
        active_backend_requests_.store(0, std::memory_order_release);
    }

    if (kind == BackendRequestKind::Streaming) {
        previous = active_streaming_requests_.fetch_sub(1, std::memory_order_acq_rel);
        if (previous <= 1) {
            active_streaming_requests_.store(0, std::memory_order_release);
        }
    } else {
        previous = active_non_streaming_requests_.fetch_sub(1, std::memory_order_acq_rel);
        if (previous <= 1) {
            active_non_streaming_requests_.store(0, std::memory_order_release);
        }
    }

    watchdog_cv_.notify_all();
}

void WrappedServer::set_watchdog_health_endpoint(const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    watchdog_policy_.health_endpoint = normalize_endpoint(endpoint);
}

void WrappedServer::configure_backend_watchdog(const BackendWatchdogPolicy& policy) {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    watchdog_policy_ = policy;
    watchdog_policy_.health_endpoint = normalize_endpoint(watchdog_policy_.health_endpoint);
}

void WrappedServer::start_backend_watchdog(const std::string& health_endpoint) {
    BackendWatchdogPolicy policy;
    policy.health_endpoint = health_endpoint;
    start_backend_watchdog(policy);
}

void WrappedServer::start_backend_watchdog(const BackendWatchdogPolicy& policy) {
    BackendWatchdogPolicy effective_policy = policy;
    effective_policy.health_endpoint = normalize_endpoint(effective_policy.health_endpoint);

    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        watchdog_policy_ = effective_policy;
    }

    if (!backend_watchdog_enabled() || !effective_policy.enabled) {
        LOG(INFO, "BackendWatchdog") << server_name_ << " watchdog disabled" << std::endl;
        return;
    }

    bool expected = false;
    if (!watchdog_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Idempotent start: keep the existing thread and just publish the new
        // policy. Do not reset active counters or watchdog state while requests
        // may already be in flight.
        watchdog_cv_.notify_all();
        return;
    }

    watchdog_triggered_.store(false, std::memory_order_release);
    watchdog_stop_requested_.store(false, std::memory_order_release);
    active_backend_requests_.store(0, std::memory_order_release);
    active_streaming_requests_.store(0, std::memory_order_release);
    active_non_streaming_requests_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        watchdog_reset_reason_.clear();
        last_backend_activity_ = std::chrono::steady_clock::now();
    }

    watchdog_thread_ = std::thread(&WrappedServer::backend_watchdog_loop, this);

    LOG(INFO, "BackendWatchdog") << "Started watchdog for " << server_name_
                                  << " using " << get_base_url() << effective_policy.health_endpoint
                                  << " (streaming=" << (effective_policy.monitor_streaming_requests ? "on" : "off")
                                  << ", non_streaming=on)" << std::endl;
}

void WrappedServer::stop_backend_watchdog() {
    if (!watchdog_running_.load(std::memory_order_acquire)) {
        active_backend_requests_.store(0, std::memory_order_release);
        active_streaming_requests_.store(0, std::memory_order_release);
        active_non_streaming_requests_.store(0, std::memory_order_release);
        return;
    }

    watchdog_stop_requested_.store(true, std::memory_order_release);
    watchdog_cv_.notify_all();

    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }

    watchdog_running_.store(false, std::memory_order_release);
    active_backend_requests_.store(0, std::memory_order_release);
    active_streaming_requests_.store(0, std::memory_order_release);
    active_non_streaming_requests_.store(0, std::memory_order_release);
}

bool WrappedServer::has_backend_process_exited() const {
    const ProcessHandle handle = get_process_handle_snapshot();
    if (!has_process_handle(handle)) {
        return false;
    }

    // Check the owned process handle/PID without probing the backend HTTP
    // endpoint. This is intentionally cheap and applies to idle, streaming, and
    // long-running non-streaming requests alike. It lets us detect a crashed or
    // zombie child without killing legitimate slow work such as image generation.
    return !utils::ProcessManager::is_running(handle);
}

void WrappedServer::request_backend_reset_from_watchdog(const std::string& reason) {
    if (watchdog_triggered_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        watchdog_reset_reason_ = reason;
    }

    // Consume the lifecycle handle exactly once. This prevents later status
    // checks or backend-specific unload() from reaping/closing the same child
    // again, and it immediately removes the stale PID/port from status output.
    const ProcessHandle handle = consume_process_handle_for_cleanup();

    if (has_process_handle(handle)) {
        if (utils::ProcessManager::is_running(handle)) {
            LOG(ERROR, "BackendWatchdog") << server_name_ << " backend marked unavailable: "
                                          << reason << "; terminating and reaping backend process PID "
                                          << handle.pid << std::endl;
            utils::ProcessManager::stop_process(handle);
        } else {
            const int exit_code = utils::ProcessManager::reap_process(handle);
            LOG(ERROR, "BackendWatchdog") << server_name_ << " backend marked unavailable: "
                                          << reason << "; reaped exited backend process PID "
                                          << handle.pid << " (exit_code=" << exit_code << ")"
                                          << std::endl;
        }
    } else {
        LOG(ERROR, "BackendWatchdog") << server_name_ << " backend marked unavailable: "
                                      << reason << "; no process handle to reap"
                                      << std::endl;
    }

    watchdog_cv_.notify_all();
}

void WrappedServer::backend_watchdog_loop() {
    const auto grace = std::chrono::seconds(
        get_env_long("LEMONADE_BACKEND_WATCHDOG_GRACE_SECONDS", 90, 10));
    const auto poll = std::chrono::seconds(
        get_env_long("LEMONADE_BACKEND_WATCHDOG_POLL_SECONDS", 5, 1));
    const int probe_timeout_seconds = static_cast<int>(
        get_env_long("LEMONADE_BACKEND_WATCHDOG_PROBE_TIMEOUT_SECONDS", 2, 1));
    const int max_failures = static_cast<int>(
        get_env_long("LEMONADE_BACKEND_WATCHDOG_MAX_FAILURES", 3, 1));

    int consecutive_failures = 0;

    while (!watchdog_stop_requested_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(watchdog_mutex_);
        watchdog_cv_.wait_for(lock, poll, [this]() {
            return watchdog_stop_requested_.load(std::memory_order_acquire);
        });

        if (watchdog_stop_requested_.load(std::memory_order_acquire) ||
            watchdog_triggered_.load(std::memory_order_acquire)) {
            break;
        }

        // Always detect child-process exit/zombie state, even when there is no
        // active request or when the active request is non-streaming. This is a
        // cheap process-handle/PID check, not an HTTP /health probe, so long
        // non-streaming work such as image generation is not killed just because
        // it takes a long time. If the backend has crashed, mark it unavailable
        // so the router can lazily reload/retry recoverable calls.
        if (has_backend_process_exited()) {
            lock.unlock();
            request_backend_reset_from_watchdog("backend process exited while watchdog was active");
            break;
        }

        const int active = active_backend_requests_.load(std::memory_order_acquire);
        if (active <= 0) {
            consecutive_failures = 0;
            continue;
        }

        const BackendWatchdogPolicy policy = watchdog_policy_;
        const bool has_streaming = active_streaming_requests_.load(std::memory_order_acquire) > 0;
        const bool has_non_streaming = active_non_streaming_requests_.load(std::memory_order_acquire) > 0;
        const bool should_monitor =
            (has_streaming && policy.monitor_streaming_requests) ||
            has_non_streaming;

        if (!policy.enabled || policy.health_endpoint.empty() || !should_monitor) {
            consecutive_failures = 0;
            continue;
        }

        const auto last_activity = last_backend_activity_;
        const auto idle_for = std::chrono::steady_clock::now() - last_activity;
        if (idle_for < grace) {
            consecutive_failures = 0;
            continue;
        }

        const std::string health_url = get_base_url() + policy.health_endpoint;
        lock.unlock();

        const ProcessHandle handle = get_process_handle_snapshot();
        if (!has_process_handle(handle) || !utils::ProcessManager::is_running(handle)) {
            request_backend_reset_from_watchdog("backend process exited during an active request");
            break;
        }

        const bool reachable = utils::HttpClient::is_reachable(
            health_url,
            probe_timeout_seconds,
            utils::HttpSecurityPolicy::TrustedLoopback);
        if (reachable) {
            consecutive_failures = 0;
            note_backend_activity();
            continue;
        }

        ++consecutive_failures;
        LOG(WARNING, "BackendWatchdog") << server_name_ << " health probe failed "
                                         << consecutive_failures << "/" << max_failures
                                         << " after "
                                         << std::chrono::duration_cast<std::chrono::seconds>(idle_for).count()
                                         << "s without observable progress"
                                         << std::endl;

        if (consecutive_failures >= max_failures) {
            request_backend_reset_from_watchdog(
                "health endpoint did not respond after " + std::to_string(max_failures) +
                " consecutive probes while a request was active");
            break;
        }
    }
}

int WrappedServer::choose_port() {
    const int chosen_port = utils::ProcessManager::find_free_port(8001);
    if (chosen_port < 0) {
        throw std::runtime_error("Failed to find free port for " + server_name_);
    }
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        port_ = chosen_port;
    }
    LOG(DEBUG, "WrappedServer") << server_name_ << " will use port: " << chosen_port << std::endl;
    return chosen_port;
}

bool WrappedServer::wait_for_ready(const std::string& endpoint, long timeout_seconds, long poll_interval_ms) {
    const std::string normalized_endpoint = normalize_endpoint(endpoint);
    std::string health_url = get_base_url() + normalized_endpoint;

    // Use global default if not specified
    if (timeout_seconds == 0) {
        timeout_seconds = utils::HttpClient::get_default_timeout();
    }

    std::cout << "Waiting for " + server_name_ + " to be ready (timeout: " << timeout_seconds << "s)..." << std::endl;
    LOG(DEBUG, "WrappedServer") << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = (timeout_seconds * 1000) / poll_interval_ms;

    for (int i = 0; i < max_attempts; i++) {
        if (load_cancel_ && load_cancel_->load()) {
            const ProcessHandle h = consume_process_handle_for_cleanup();
            if (has_process_handle(h)) utils::ProcessManager::stop_process(h);
            LOG(WARNING, "WrappedServer") << server_name_ << " load cancelled" << std::endl;
            return false;
        }

        // Check if process is still running. If it already exited, consume and
        // reap the owned handle here so the caller cannot later signal a stale
        // PID while cleaning up a failed startup.
        const ProcessHandle handle = get_process_handle_snapshot();
        if (!has_process_handle(handle) || !utils::ProcessManager::is_running(handle)) {
            const ProcessHandle exited_handle = consume_process_handle_for_cleanup();
            int exit_code = has_process_handle(exited_handle)
                ? utils::ProcessManager::reap_process(exited_handle)
                : -1;
            LOG(ERROR, "WrappedServer") << server_name_ << " process has terminated with exit code: "
                     << exit_code << std::endl;
            LOG(ERROR, "WrappedServer") << "This usually means:" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Missing required drivers or dependencies" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Incompatible model file" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Try running the server manually to see the actual error" << std::endl;
            return false;
        }

        // Try health endpoint
        if (utils::HttpClient::is_reachable(
                health_url, 1, utils::HttpSecurityPolicy::TrustedLoopback)) {
            LOG(INFO, "WrappedServer") << server_name_ + " is ready!" << std::endl;
            start_backend_watchdog(normalized_endpoint);
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));

        // Print progress every 10 seconds
        if (i % 100 == 0 && i > 0) {
            LOG(DEBUG, "WrappedServer") << "Still waiting for " + server_name_ + "..." << std::endl;
        }
    }

    LOG(ERROR, "WrappedServer") << server_name_ + " failed to start within timeout" << std::endl;
    return false;
}

bool WrappedServer::is_process_running() const {
    return is_backend_alive();
}

json WrappedServer::forward_get_request(const std::string& endpoint, long timeout_seconds) {
    (void)timeout_seconds;
    if (!is_backend_alive()) {
        if (was_watchdog_triggered() || has_backend_process_exited()) {
            if (!was_watchdog_triggered()) {
                request_backend_reset_from_watchdog("backend process exited before request");
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::NonStreaming);

    std::string url = get_base_url() + endpoint;
    std::map<std::string, std::string> headers;

    try {
        auto response = utils::HttpClient::get(url, headers, 0,
                                               utils::HttpSecurityPolicy::TrustedLoopback);
        note_backend_activity();

        if (response.status_code == 200) {
            return json::parse(response.body);
        }

        json error_details;
        try {
            error_details = json::parse(response.body);
        } catch (...) {
            error_details = response.body;
        }

        return create_backend_error_response(server_name_, response.status_code, error_details);
    } catch (const std::exception& e) {
        if (was_watchdog_triggered() || has_backend_process_exited() || is_backend_connection_failure(e.what())) {
            if (!was_watchdog_triggered()) {
                const std::string reset_reason = has_backend_process_exited()
                    ? "backend process exited during request"
                    : "backend connection failed during request: " + std::string(e.what());
                request_backend_reset_from_watchdog(reset_reason);
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

json WrappedServer::forward_request(const std::string& endpoint, const json& request, long timeout_seconds) {
    if (!is_backend_alive()) {
        if (was_watchdog_triggered() || has_backend_process_exited()) {
            if (!was_watchdog_triggered()) {
                request_backend_reset_from_watchdog("backend process exited before request");
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::NonStreaming);

    std::string url = get_base_url() + endpoint;
    std::map<std::string, std::string> headers = {{"Content-Type", "application/json"}};

    try {
        auto response = utils::HttpClient::post(
            url,
            request.dump(),
            headers,
            timeout_seconds,
            utils::HttpSecurityPolicy::TrustedLoopback,
            current_request_cancel());
        note_backend_activity();

        if (response.status_code == 200) {
            return json::parse(response.body);
        } else {
            // Try to parse error response from backend
            json error_details;
            try {
                error_details = json::parse(response.body);
            } catch (...) {
                error_details = response.body;
            }

            return create_backend_error_response(
                server_name_,
                response.status_code,
                error_details
            );
        }
    } catch (const std::exception& e) {
        if (was_watchdog_triggered() || has_backend_process_exited() || is_backend_connection_failure(e.what())) {
            if (!was_watchdog_triggered()) {
                const std::string reset_reason = has_backend_process_exited()
                    ? "backend process exited during request"
                    : "backend connection failed during request: " + std::string(e.what());
                request_backend_reset_from_watchdog(reset_reason);
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

json WrappedServer::forward_multipart_request(const std::string& endpoint,
                                               const std::vector<utils::MultipartField>& fields,
                                               long timeout_seconds) {
    if (!is_backend_alive()) {
        if (was_watchdog_triggered() || has_backend_process_exited()) {
            if (!was_watchdog_triggered()) {
                request_backend_reset_from_watchdog("backend process exited before request");
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::NonStreaming);

    std::string url = get_base_url() + endpoint;

    try {
        auto response = utils::HttpClient::post_multipart(
            url,
            fields,
            timeout_seconds,
            utils::HttpSecurityPolicy::TrustedLoopback);
        note_backend_activity();

        if (response.status_code == 200) {
            return json::parse(response.body);
        } else {
            LOG(ERROR, "WrappedServer") << "Backend returned HTTP " << response.status_code
                      << " for multipart request: " << response.body << std::endl;
            json error_details;
            try {
                error_details = json::parse(response.body);
            } catch (...) {
                error_details = response.body;
            }

            return ErrorResponse::create(
                server_name_ + " request failed",
                ErrorType::BACKEND_ERROR,
                {
                    {"status_code", response.status_code},
                    {"response", error_details}
                }
            );
        }
    } catch (const std::exception& e) {
        if (was_watchdog_triggered() || has_backend_process_exited() || is_backend_connection_failure(e.what())) {
            if (!was_watchdog_triggered()) {
                const std::string reset_reason = has_backend_process_exited()
                    ? "backend process exited during request"
                    : "backend connection failed during request: " + std::string(e.what());
                request_backend_reset_from_watchdog(reset_reason);
            }
            return create_watchdog_reset_response();
        }
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

void WrappedServer::forward_streaming_request(const std::string& endpoint,
                                              const std::string& request_body,
                                              httplib::DataSink& sink,
                                              bool sse,
                                              long timeout_seconds,
                                              TelemetryCallback telemetry_callback) {
    if (!is_backend_alive()) {
        if (was_watchdog_triggered() || has_backend_process_exited()) {
            if (!was_watchdog_triggered()) {
                request_backend_reset_from_watchdog("backend process exited before streaming request");
            }
            throw BackendStreamRetryableReset(get_watchdog_reset_reason());
        }

        json error = ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
        std::string error_msg = "data: " + error.dump() + "\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::Streaming);

    std::string url = get_base_url() + endpoint;
    bool streamed_any_bytes = false;
    auto mark_stream_progress = [this, &streamed_any_bytes]() {
        streamed_any_bytes = true;
        note_backend_activity();
    };

    try {

        if (sse) {
            // Use StreamingProxy to forward the SSE stream with telemetry callback
            // Use INFERENCE_TIMEOUT_SECONDS (0 = infinite) as chat completions can take a long time
            StreamingProxy::forward_sse_stream(url, request_body, sink,
                [telemetry_callback](const StreamingProxy::TelemetryData& telemetry) {
                    if (telemetry_callback) {
                        telemetry_callback(telemetry.input_tokens,
                                           telemetry.output_tokens,
                                           telemetry.time_to_first_token,
                                           telemetry.tokens_per_second,
                                           telemetry.error_message);
                    }
                },
                timeout_seconds,
                mark_stream_progress
            );
        } else {
            StreamingProxy::forward_byte_stream(url, request_body, sink, timeout_seconds,
                mark_stream_progress
            );
        }
    } catch (const std::exception& e) {
        // Log the error but don't crash the server
        LOG(ERROR, "WrappedServer") << "Streaming request failed: " << e.what() << std::endl;

        bool will_retry = (was_watchdog_triggered() || has_backend_process_exited() || is_backend_connection_failure(e.what())) && !streamed_any_bytes;

        if (telemetry_callback && !will_retry) {
            telemetry_callback(0, 0, 0.0, 0.0, e.what());
        }

        // Try to send error to client if possible
        try {
            json error;
            if (was_watchdog_triggered() || has_backend_process_exited() || is_backend_connection_failure(e.what())) {
                if (!was_watchdog_triggered()) {
                    const std::string reset_reason = has_backend_process_exited()
                        ? "backend process exited during streaming request"
                        : "backend connection failed during streaming request: " + std::string(e.what());
                    request_backend_reset_from_watchdog(reset_reason);
                }

                // If the backend died before the client received any stream bytes,
                // let the router reload and replay the streaming request once. Once
                // bytes have reached the client, replaying could duplicate tokens or
                // corrupt the SSE protocol, so we only reload for the next request.
                if (!streamed_any_bytes) {
                    throw BackendStreamRetryableReset(get_watchdog_reset_reason());
                }

                error = create_watchdog_reset_response();
            } else {
                error = ErrorResponse::create(std::string(e.what()), "streaming_error");
            }
            std::string error_msg = "data: " + error.dump() + "\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
        } catch (const BackendStreamRetryableReset&) {
            throw;
        } catch (...) {
            // Sink might be closed, ignore
        }
    }
}

} // namespace lemon
