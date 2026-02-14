#include <lemon/wrapped_server.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/http_client.h>
#include <lemon/streaming_proxy.h>
#include <lemon/error_types.h>
#include <httplib.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>

namespace lemon {

int WrappedServer::choose_port() {
    port_ = utils::ProcessManager::find_free_port(8001);
    if (port_ < 0) {
        throw std::runtime_error("Failed to find free port for " + server_name_);
    }
    std::cout << server_name_ << " will use port: " << port_ << std::endl;
    return port_;
}

bool WrappedServer::wait_for_ready(const std::string& endpoint, long timeout_seconds, long poll_interval_ms) {
    std::string health_url = get_base_url() + endpoint;

    std::cout << "Waiting for " + server_name_ + " to be ready..." << std::endl;

    const int max_attempts = (timeout_seconds * 1000) / poll_interval_ms;

    for (int i = 0; i < max_attempts; i++) {
        // Check if process is still running
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            std::cerr << "[ERROR] " << server_name_ << " process has terminated with exit code: "
                     << exit_code << std::endl;
            std::cerr << "[ERROR] This usually means:" << std::endl;
            std::cerr << "  - Missing required drivers or dependencies" << std::endl;
            std::cerr << "  - Incompatible model file" << std::endl;
            std::cerr << "  - Try running the server manually to see the actual error" << std::endl;
            return false;
        }

        // Try both health endpoints
        if (utils::HttpClient::is_reachable(health_url, 1)) {
            std::cout << server_name_ + " is ready!" << std::endl;
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));

        // Print progress every 10 seconds
        if (i % 100 == 0 && i > 0) {
            std::cout << "Still waiting for " + server_name_ + "..." << std::endl;
        }
    }

    std::cerr << server_name_ + " failed to start within timeout" << std::endl;
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
        auto response = utils::HttpClient::post(url, request.dump(), headers, timeout_seconds);

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

json WrappedServer::forward_multipart_request(const std::string& endpoint, const json& request, long timeout_seconds) {
    if (!is_process_running()) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }

    try {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_read_timeout(timeout_seconds, 0);
        cli.set_write_timeout(timeout_seconds, 0);

        httplib::UploadFormDataItems items;

        // Helper lambda to decode base64
        auto base64_decode = [](const std::string& encoded_string) -> std::string {
            static const std::string base64_chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";

            std::string decoded;
            std::vector<int> T(256, -1);
            for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

            int val = 0, valb = -8;
            for (unsigned char c : encoded_string) {
                if (T[c] == -1) break;
                val = (val << 6) + T[c];
                valb += 6;
                if (valb >= 0) {
                    decoded.push_back(char((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return decoded;
        };

        // Add image file
        if (request.contains("image_data") && request.contains("image_filename")) {
            std::string image_data = base64_decode(request["image_data"].get<std::string>());
            std::string filename = request["image_filename"].get<std::string>();
            items.push_back({"image[]", image_data, filename, "image/png"});
        }

        // Add mask file if present
        if (request.contains("mask_data") && request.contains("mask_filename")) {
            std::string mask_data = base64_decode(request["mask_data"].get<std::string>());
            std::string filename = request["mask_filename"].get<std::string>();
            items.push_back({"mask[]", mask_data, filename, "image/png"});
        }

        // Add text fields
        if (request.contains("prompt")) {
            items.push_back({"prompt", request["prompt"].get<std::string>(), "", ""});
        }
        if (request.contains("model")) {
            items.push_back({"model", request["model"].get<std::string>(), "", ""});
        }
        if (request.contains("n")) {
            items.push_back({"n", std::to_string(request["n"].get<int>()), "", ""});
        }
        if (request.contains("size")) {
            items.push_back({"size", request["size"].get<std::string>(), "", ""});
        }
        if (request.contains("response_format")) {
            items.push_back({"response_format", request["response_format"].get<std::string>(), "", ""});
        }
        if (request.contains("user")) {
            items.push_back({"user", request["user"].get<std::string>(), "", ""});
        }

        // Add SD-specific parameters
        if (request.contains("steps")) {
            items.push_back({"steps", std::to_string(request["steps"].get<int>()), "", ""});
        }
        if (request.contains("cfg_scale")) {
            items.push_back({"cfg_scale", std::to_string(request["cfg_scale"].get<double>()), "", ""});
        }
        if (request.contains("seed")) {
            items.push_back({"seed", std::to_string(request["seed"].get<long long>()), "", ""});
        }
        if (request.contains("sample_method")) {
            items.push_back({"sample_method", request["sample_method"].get<std::string>(), "", ""});
        }
        if (request.contains("scheduler")) {
            items.push_back({"scheduler", request["scheduler"].get<std::string>(), "", ""});
        }

        auto res = cli.Post(endpoint, items);

        if (res && res->status == 200) {
            return json::parse(res->body);
        } else if (res) {
            // Try to parse error response from backend
            json error_details;
            try {
                error_details = json::parse(res->body);
            } catch (...) {
                error_details = res->body;
            }

            json details = json::object();
            details["status_code"] = res->status;
            details["response"] = error_details;

            return ErrorResponse::create(
                server_name_ + " request failed",
                ErrorType::BACKEND_ERROR,
                details
            );
        } else {
            json details = json::object();
            details["error"] = "Could not connect to backend server";

            return ErrorResponse::create(
                server_name_ + " connection failed",
                ErrorType::NETWORK_ERROR,
                details
            );
        }
    } catch (const std::exception& e) {
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

void WrappedServer::forward_streaming_request(const std::string& endpoint,
                                              const std::string& request_body,
                                              httplib::DataSink& sink,
                                              bool sse) {
    if (!is_process_running()) {
        std::string error_msg = "data: {\"error\":{\"message\":\"No model loaded: " + server_name_ +
                               "\",\"type\":\"model_not_loaded\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
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
                INFERENCE_TIMEOUT_SECONDS
            );
        } else {
            StreamingProxy::forward_byte_stream(url, request_body, sink, INFERENCE_TIMEOUT_SECONDS);
        }
    } catch (const std::exception& e) {
        // Log the error but don't crash the server
        std::cerr << "[WrappedServer ERROR] Streaming request failed: " << e.what() << std::endl;
        // Try to send error to client if possible
        try {
            std::string error_msg = "data: {\"error\":{\"message\":\"" + std::string(e.what()) +
                                   "\",\"type\":\"streaming_error\"}}\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
        } catch (...) {
            // Sink might be closed, ignore
        }
    }
}

} // namespace lemon
