#include "lemon/streaming_proxy.h"
#include <sstream>
#include <iostream>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <curl/curl.h>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

void extract_telemetry_from_chunk(const nlohmann::json& chunk, StreamingProxy::TelemetryData& telemetry) {
    nlohmann::json usage;
    if (chunk.contains("usage")) {
        usage = chunk["usage"];
    } else if (chunk.contains("response") && chunk["response"].is_object() && chunk["response"].contains("usage")) {
        usage = chunk["response"]["usage"];
    }

    if (usage.is_object()) {
        if (usage.contains("prompt_tokens")) {
            telemetry.input_tokens = usage["prompt_tokens"].get<int>();
        } else if (usage.contains("input_tokens")) {
            telemetry.input_tokens = usage["input_tokens"].get<int>();
        }
        if (usage.contains("completion_tokens")) {
            telemetry.output_tokens = usage["completion_tokens"].get<int>();
        } else if (usage.contains("output_tokens")) {
            telemetry.output_tokens = usage["output_tokens"].get<int>();
        }
        if (usage.contains("prefill_duration_ttft")) {
            telemetry.time_to_first_token = usage["prefill_duration_ttft"].get<double>();
        }
        if (usage.contains("decoding_speed_tps")) {
            telemetry.tokens_per_second = usage["decoding_speed_tps"].get<double>();
        }
    }

    nlohmann::json timings;
    if (chunk.contains("timings")) {
        timings = chunk["timings"];
    } else if (chunk.contains("response") && chunk["response"].is_object() && chunk["response"].contains("timings")) {
        timings = chunk["response"]["timings"];
    }

    if (timings.is_object()) {
        if (timings.contains("prompt_n")) {
            telemetry.input_tokens = timings["prompt_n"].get<int>();
        }
        if (timings.contains("predicted_n")) {
            telemetry.output_tokens = timings["predicted_n"].get<int>();
        }
        if (timings.contains("prompt_ms")) {
            telemetry.time_to_first_token = timings["prompt_ms"].get<double>() / 1000.0;
        }
        if (timings.contains("predicted_per_second")) {
            telemetry.tokens_per_second = timings["predicted_per_second"].get<double>();
        }
    }
}

} // namespace


void StreamingProxy::forward_sse_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds,
    std::function<void()> on_chunk) {

    TelemetryData telemetry;
    try {
        auto req_json = json::parse(request_body);
        if (req_json.contains("model") && req_json["model"].is_string()) {
            telemetry.model_name = req_json["model"].get<std::string>();
        }
    } catch (...) {}
    std::string line_buffer;
    bool stream_error = false;
    bool has_done_marker = false;
    bool has_first_token = false;
    double time_to_first_token = 0.0;
    const auto start_time = std::chrono::steady_clock::now();

    auto process_line = [&telemetry](const std::string& line) {
        std::string json_str;
        if (line.find("data: ") == 0) {
            json_str = line.substr(6);
        } else if (line.find("ChatCompletionChunk: ") == 0) {
            json_str = line.substr(21);
        }
        if (!json_str.empty() && json_str != "[DONE]") {
            try {
                auto chunk = json::parse(json_str);
                extract_telemetry_from_chunk(chunk, telemetry);
            } catch (...) {}
        }
    };

    utils::HttpResponse result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &line_buffer, &has_done_marker, &has_first_token,
         &time_to_first_token, &start_time, &on_chunk, &process_line](const char* data, size_t length) {
            if (on_chunk) {
                on_chunk();
            }

            line_buffer.append(data, length);
            process_sse_lines(line_buffer, process_line);

            std::string chunk(data, length);
            if (!has_first_token && chunk.find("data: ") != std::string::npos) {
                has_first_token = true;
                time_to_first_token = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_time).count();
            }

            if (chunk.find("data: [DONE]") != std::string::npos) {
                has_done_marker = true;
            }

            if (!sink.write(data, length)) {
                return false;
            }

            return true;
        },
        {},
        timeout_seconds,
        nullptr,
        utils::HttpSecurityPolicy::TrustedLoopback
    );

    const bool transport_interrupted =
        result.curl_code == CURLE_PARTIAL_FILE || result.curl_code == CURLE_RECV_ERROR;

    if (result.curl_code != CURLE_OK) {
        if (result.curl_code == CURLE_WRITE_ERROR) {
            stream_error = true;
            LOG(WARNING, "StreamingProxy") << "Client disconnected during SSE stream (CURL error: " << result.curl_error << ")" << std::endl;
            telemetry.error_message = "Client disconnected during stream";
        } else if (transport_interrupted) {
            if (!has_done_marker) {
                // This is the important crash path: HTTP headers may have been sent and
                // some bytes may even have reached the client, but the SSE protocol never
                // completed. Do not synthesize [DONE], because that hides backend crashes
                // from the router and leaves stale loaded-model state behind.
                throw std::runtime_error(
                    "backend connection failed during SSE stream before DONE: CURL error: " +
                    result.curl_error);
            }
        } else {
            stream_error = true;
            LOG(ERROR, "StreamingProxy") << "SSE stream failed: CURL error: " << result.curl_error << std::endl;
            telemetry.error_message = "SSE stream failed: CURL error: " + result.curl_error;
        }
    }

    if (result.status_code != 200) {
        stream_error = true;
        LOG(ERROR, "StreamingProxy") << "Backend returned error: " << result.status_code << std::endl;
        telemetry.error_message = "Backend returned error status code: " + std::to_string(result.status_code);
    }

    if (!stream_error) {
        // Ensure [DONE] marker is sent only for clean transports. If the transport
        // was interrupted before [DONE], the block above throws and recovery is
        // handled by WrappedServer/Router instead of pretending success.
        if (!has_done_marker) {
            LOG(WARNING, "StreamingProxy") << "WARNING: Backend did not send [DONE] marker, adding it" << std::endl;
            const char* done_marker = "data: [DONE]\n\n";
            sink.write(done_marker, strlen(done_marker));
        }

        sink.done();

        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;

        if (!line_buffer.empty()) {
            if (line_buffer.back() == '\r') {
                line_buffer.pop_back();
            }
            process_line(line_buffer);
        }

        if (telemetry.time_to_first_token <= 0.0) {
            telemetry.time_to_first_token = time_to_first_token;
        }
        if (telemetry.tokens_per_second <= 0.0 && telemetry.output_tokens > 0) {
            double total_duration = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time).count();
            double decode_duration = total_duration - telemetry.time_to_first_token;
            if (decode_duration > 0.0) {
                telemetry.tokens_per_second = telemetry.output_tokens / decode_duration;
            }
        }
        telemetry.print();

        if (on_complete) {
            on_complete(telemetry);
        }
    } else {
        sink.done();
        if (on_complete) {
            on_complete(telemetry);
        }
    }
}

void StreamingProxy::forward_byte_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    long timeout_seconds,
    std::function<void()> on_chunk) {

    bool stream_error = false;

    // On a non-200 the backend body is an error description, not payload bytes:
    // divert it here instead of the client sink, so it can be reshaped into a
    // JSON error below rather than served as successful media.
    int backend_status = 200;
    std::string error_body;
    static constexpr size_t max_error_body = 64 * 1024;

    utils::HttpResponse result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &on_chunk, &backend_status, &error_body](const char* data, size_t length) {
            if (on_chunk) {
                on_chunk();
            }

            if (backend_status != 200) {
                if (error_body.size() < max_error_body) {
                    error_body.append(data, std::min(length, max_error_body - error_body.size()));
                }
                return true;
            }

            if (!sink.write(data, length)) {
                return false;
            }

            return true;
        },
        {},
        timeout_seconds,
        [&backend_status](int status) { backend_status = status; },
        utils::HttpSecurityPolicy::TrustedLoopback
    );

    const bool transport_interrupted =
        result.curl_code == CURLE_PARTIAL_FILE || result.curl_code == CURLE_RECV_ERROR;

    if (result.curl_code != CURLE_OK) {
        stream_error = true;
        if (result.curl_code == CURLE_WRITE_ERROR) {
            LOG(WARNING, "StreamingProxy") << "Client disconnected during byte stream (CURL error: " << result.curl_error << ")" << std::endl;
        } else if (transport_interrupted) {
            // Keep byte streams consistent with SSE: an interrupted transport is a
            // backend failure, not a clean stream completion. The caller will mark
            // the backend unavailable and reload after the current response unwinds.
            throw std::runtime_error(
                "backend connection failed during byte stream: CURL error: " +
                result.curl_error);
        } else {
            LOG(ERROR, "StreamingProxy") << "Byte stream failed: CURL error: " << result.curl_error << std::endl;
        }
    }

    if (result.status_code != 200 || backend_status != 200) {
        stream_error = true;
        const int status = backend_status != 200 ? backend_status : result.status_code;
        LOG(ERROR, "StreamingProxy") << "Backend returned error " << status
                                     << (error_body.empty() ? "" : ": " + error_body) << std::endl;

        json payload;
        try {
            payload = json::parse(error_body);
        } catch (...) {
            payload = nullptr;
        }
        if (!payload.is_object() || !payload.contains("error")) {
            std::string message = error_body.empty()
                ? "backend returned HTTP " + std::to_string(status)
                : error_body;
            payload = json{{"error", {{"message", message},
                                      {"type", "backend_error"},
                                      {"status", status}}}};
        }
        const std::string out = payload.dump();
        sink.write(out.data(), out.size());
    }

    if (!stream_error) {
        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
    }
    sink.done();
}

StreamingProxy::TelemetryData StreamingProxy::parse_telemetry(const std::string& buffer) {
    TelemetryData telemetry;

    std::istringstream stream(buffer);
    std::string line;
    json last_chunk_with_usage;

    while (std::getline(stream, line)) {
        std::string json_str;
        if (line.find("data: ") == 0) {
            json_str = line.substr(6);
        } else if (line.find("ChatCompletionChunk: ") == 0) {
            json_str = line.substr(21);
        }

        if (!json_str.empty() && json_str != "[DONE]") {
            try {
                auto chunk = json::parse(json_str);
                bool has_usage = chunk.contains("usage") || chunk.contains("timings");
                if (!has_usage && chunk.contains("response") && chunk["response"].is_object()) {
                    has_usage = chunk["response"].contains("usage") || chunk["response"].contains("timings");
                }
                if (has_usage) {
                    last_chunk_with_usage = chunk;
                }
            } catch (...) {}
        }
    }

    if (!last_chunk_with_usage.empty()) {
        try {
            extract_telemetry_from_chunk(last_chunk_with_usage, telemetry);
        } catch (const std::exception& e) {
            LOG(ERROR, "StreamingProxy") << "Error parsing telemetry: " << e.what() << std::endl;
        }
    }

    return telemetry;
}

void StreamingProxy::process_sse_lines(std::string& line_buffer, std::function<void(const std::string&)> line_callback) {
    size_t pos;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
        std::string line = line_buffer.substr(0, pos);
        line_buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line_callback(line);
    }
}

void StreamingProxy::accumulate_responses_delta(const nlohmann::json& parsed, std::string& accumulated_text) {
    if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
        auto choice = parsed["choices"][0];
        if (choice.is_object() && choice.contains("delta")) {
            auto delta = choice["delta"];
            if (delta.is_object() && delta.contains("content") && delta["content"].is_string()) {
                accumulated_text += delta["content"].get<std::string>();
            }
        }
    }
    if (parsed.contains("response") && parsed["response"].is_string()) {
        accumulated_text += parsed["response"].get<std::string>();
    }
    // Supports Responses API type-restricted delta vs. backward compatible fallback.
    if (parsed.contains("delta")) {
        bool should_extract_delta = true;
        if (parsed.contains("type")) {
            should_extract_delta = (parsed["type"] == "response.output_text.delta");
        }
        if (should_extract_delta) {
            if (parsed["delta"].is_string()) {
                accumulated_text += parsed["delta"].get<std::string>();
            } else if (parsed["delta"].is_object() && parsed["delta"].contains("text") && parsed["delta"]["text"].is_string()) {
                accumulated_text += parsed["delta"]["text"].get<std::string>();
            }
        }
    }
}

} // namespace lemon
