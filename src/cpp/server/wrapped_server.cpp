#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
#include <lemon/streaming_proxy.h>
#include <lemon/error_types.h>
#include <httplib.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

int WrappedServer::choose_port() {
    port_ = utils::ProcessManager::find_free_port(8001);
    if (port_ < 0) {
        throw std::runtime_error("Failed to find free port for " + server_name_);
    }
    LOG(DEBUG, "WrappedServer") << server_name_ << " will use port: " << port_ << std::endl;
    return port_;
}

bool WrappedServer::wait_for_ready(const std::string& endpoint, long timeout_seconds, long poll_interval_ms) {
    std::string health_url = get_base_url() + endpoint;

    // Use global default if not specified
    if (timeout_seconds == 0) {
        timeout_seconds = utils::HttpClient::get_default_timeout();
    }

    std::cout << "Waiting for " + server_name_ + " to be ready (timeout: " << timeout_seconds << "s)..." << std::endl;
    LOG(DEBUG, "WrappedServer") << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = (timeout_seconds * 1000) / poll_interval_ms;

    for (int i = 0; i < max_attempts; i++) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            LOG(ERROR, "WrappedServer") << server_name_ << " process has terminated with exit code: "
                     << exit_code << std::endl;
            LOG(ERROR, "WrappedServer") << "This usually means:" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Missing required drivers or dependencies" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Incompatible model file" << std::endl;
            LOG(ERROR, "WrappedServer") << "  - Try running the server manually to see the actual error" << std::endl;
            return false;
        }

        // Try both health endpoints
        if (utils::HttpClient::is_reachable(health_url, 1)) {
            LOG(INFO, "WrappedServer") << server_name_ + " is ready!" << std::endl;
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
#ifdef _WIN32
    return process_handle_.handle != nullptr;
#else
    return process_handle_.pid > 0;
#endif
}

json WrappedServer::forward_request(const std::string& endpoint, const json& request, long timeout_seconds) {
    if (!is_process_running()) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    std::string url = get_base_url() + endpoint;
    std::map<std::string, std::string> headers = {{"Content-Type", "application/json"}};

    try {
        auto response = utils::HttpClient::post(url, request.dump(), headers,
                                               timeout_seconds);

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
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

json WrappedServer::forward_multipart_request(const std::string& endpoint,
                                               const std::vector<utils::MultipartField>& fields,
                                               long timeout_seconds) {
    if (!is_process_running()) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    std::string url = get_base_url() + endpoint;

    try {
        auto response = utils::HttpClient::post_multipart(url, fields,
                                                         timeout_seconds);

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
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

void WrappedServer::forward_streaming_request(const std::string& endpoint,
                                              const std::string& request_body,
                                              httplib::DataSink& sink,
                                              bool sse,
                                              long timeout_seconds) {
    if (!is_process_running()) {
        std::string error_msg = "data: {\"error\":{\"message\":\"No model loaded: " + server_name_ +
                               "\",\"type\":\"model_not_loaded\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    std::string url = get_base_url() + endpoint;

    try {

        if (sse) {
            // Use StreamingProxy to forward the SSE stream with telemetry callback
            // Use INFERENCE_TIMEOUT_SECONDS (0 = infinite) as chat completions can take a long time
            StreamingProxy::forward_sse_stream(url, request_body, sink,
                [this](const StreamingProxy::TelemetryData& telemetry) {
                    // Save telemetry to member variable
                    telemetry_.input_tokens = telemetry.input_tokens;
                    telemetry_.output_tokens = telemetry.output_tokens;
                    telemetry_.time_to_first_token = telemetry.time_to_first_token;
                    telemetry_.tokens_per_second = telemetry.tokens_per_second;
                    // Note: decode_token_times is not available from streaming proxy
                },
                timeout_seconds
            );
        } else {
            StreamingProxy::forward_byte_stream(url, request_body, sink, timeout_seconds);
        }
    } catch (const std::exception& e) {
        // Log the error but don't crash the server
        LOG(ERROR, "WrappedServer") << "Streaming request failed: " << e.what() << std::endl;
        // Try to send error to client if possible
        try {
            std::string error_msg = "data: {\"error\":{\"message\":\"" + std::string(e.what()) +
                                   "\",\"type\":\"streaming_error\"}}\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
        } catch (...) {
            // Sink might be closed, ignore
        }
    }
}

} // namespace lemon
