#include "lemon/streaming_proxy.h"
#include <sstream>
#include <iostream>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <curl/curl.h>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

void StreamingProxy::forward_sse_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds,
    std::function<void()> on_chunk) {

    TelemetryData telemetry;
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
                if (chunk.contains("usage")) {
                    auto usage = chunk["usage"];
                    if (usage.contains("prompt_tokens")) {
                        telemetry.input_tokens = usage["prompt_tokens"].get<int>();
                    }
                    if (usage.contains("completion_tokens")) {
                        telemetry.output_tokens = usage["completion_tokens"].get<int>();
                    }
                    if (usage.contains("prefill_duration_ttft")) {
                        telemetry.time_to_first_token = usage["prefill_duration_ttft"].get<double>();
                    }
                    if (usage.contains("decoding_speed_tps")) {
                        telemetry.tokens_per_second = usage["decoding_speed_tps"].get<double>();
                    }
                }
                if (chunk.contains("timings")) {
                    auto timings = chunk["timings"];
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
            } catch (...) {}
        }
    };

    auto result = utils::HttpClient::post_stream(
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
        timeout_seconds
    );

    const bool transport_interrupted =
        result.curl_code == CURLE_PARTIAL_FILE || result.curl_code == CURLE_RECV_ERROR;

    if (result.status_code != 200) {
        stream_error = true;
        LOG(ERROR, "StreamingProxy") << "Backend returned error: " << result.status_code << std::endl;
        telemetry.error_message = "Backend returned error status code: " + std::to_string(result.status_code);
    }

    if (transport_interrupted && !has_done_marker) {
        // This is the important crash path: HTTP headers may have been sent and
        // some bytes may even have reached the client, but the SSE protocol never
        // completed. Do not synthesize [DONE], because that hides backend crashes
        // from the router and leaves stale loaded-model state behind.
        stream_error = true;
        throw std::runtime_error(
            "backend connection failed during SSE stream before DONE: CURL error: " +
            result.curl_error);
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

    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &on_chunk](const char* data, size_t length) {
            if (on_chunk) {
                on_chunk();
            }

            if (!sink.write(data, length)) {
                return false;
            }

            return true;
        },
        {},
        timeout_seconds
    );

    const bool transport_interrupted =
        result.curl_code == CURLE_PARTIAL_FILE || result.curl_code == CURLE_RECV_ERROR;

    if (result.status_code != 200) {
        stream_error = true;
        LOG(ERROR, "StreamingProxy") << "Backend returned error: " << result.status_code << std::endl;
    }

    if (transport_interrupted) {
        // Keep byte streams consistent with SSE: an interrupted transport is a
        // backend failure, not a clean stream completion. The caller will mark
        // the backend unavailable and reload after the current response unwinds.
        stream_error = true;
        throw std::runtime_error(
            "backend connection failed during byte stream: CURL error: " +
            result.curl_error);
    }

    if (!stream_error) {
        sink.done();
        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
    } else {
        sink.done();
    }
}

StreamingProxy::TelemetryData StreamingProxy::parse_telemetry(const std::string& buffer) {
    TelemetryData telemetry;

    std::istringstream stream(buffer);
    std::string line;
    json last_chunk_with_usage;

    while (std::getline(stream, line)) {
        // Handle SSE format (data: ...)
        std::string json_str;
        if (line.find("data: ") == 0) {
            json_str = line.substr(6); // Remove "data: " prefix
        } else if (line.find("ChatCompletionChunk: ") == 0) {
            // FLM debug format
            json_str = line.substr(21); // Remove "ChatCompletionChunk: " prefix
        }

        if (!json_str.empty() && json_str != "[DONE]") {
            try {
                auto chunk = json::parse(json_str);
                // Look for usage or timings in the chunk
                if (chunk.contains("usage") || chunk.contains("timings")) {
                    last_chunk_with_usage = chunk;
                }
            } catch (...) {
                // Skip invalid JSON
            }
        }
    }

    // Extract telemetry from the last chunk with usage data
    if (!last_chunk_with_usage.empty()) {
        try {
            if (last_chunk_with_usage.contains("usage")) {
                auto usage = last_chunk_with_usage["usage"];

                if (usage.contains("prompt_tokens")) {
                    telemetry.input_tokens = usage["prompt_tokens"].get<int>();
                }
                if (usage.contains("completion_tokens")) {
                    telemetry.output_tokens = usage["completion_tokens"].get<int>();
                }

                // FLM format
                if (usage.contains("prefill_duration_ttft")) {
                    telemetry.time_to_first_token = usage["prefill_duration_ttft"].get<double>();
                }
                if (usage.contains("decoding_speed_tps")) {
                    telemetry.tokens_per_second = usage["decoding_speed_tps"].get<double>();
                }
            }

            // Alternative format (timings)
            if (last_chunk_with_usage.contains("timings")) {
                auto timings = last_chunk_with_usage["timings"];

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

} // namespace lemon
