#include "lemon/streaming_proxy.h"
#include <chrono>
#include <sstream>
#include <iostream>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

void StreamingProxy::forward_sse_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds) {

    std::string telemetry_buffer;
    bool stream_error = false;
    bool has_done_marker = false;
    std::vector<double> decode_times;
    std::string sse_parse_buffer;
    std::chrono::steady_clock::time_point last_token_time;
    bool first_token_received = false;

    // Use HttpClient to stream from backend
    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &telemetry_buffer, &has_done_marker, &decode_times,
         &sse_parse_buffer, &last_token_time, &first_token_received](const char* data, size_t length) {
            // Buffer for telemetry parsing
            telemetry_buffer.append(data, length);

            // Check if this chunk contains [DONE]
            std::string chunk(data, length);
            if (chunk.find("[DONE]") != std::string::npos) {
                has_done_marker = true;
            }

            sse_parse_buffer.append(data, length);
            size_t line_end = 0;
            while ((line_end = sse_parse_buffer.find('\n')) != std::string::npos) {
                std::string line = sse_parse_buffer.substr(0, line_end);
                sse_parse_buffer.erase(0, line_end + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                std::string json_str;
                if (line.find("data: ") == 0) {
                    json_str = line.substr(6);
                } else if (line.find("ChatCompletionChunk: ") == 0) {
                    json_str = line.substr(21);
                }

                if (json_str.empty() || json_str == "[DONE]") {
                    continue;
                }

                try {
                    auto chunk_json = json::parse(json_str);
                    if (!chunk_json.contains("choices") || !chunk_json["choices"].is_array() ||
                        chunk_json["choices"].empty()) {
                        continue;
                    }

                    const auto& choice = chunk_json["choices"][0];
                    if (!choice.contains("delta") || !choice["delta"].is_object()) {
                        continue;
                    }

                    const auto& delta = choice["delta"];
                    bool has_token_content = false;
                    for (const auto& field : {"content", "reasoning_content"}) {
                        if (delta.contains(field) && delta[field].is_string() &&
                            !delta[field].get<std::string>().empty()) {
                            has_token_content = true;
                            break;
                        }
                    }

                    if (!has_token_content) {
                        continue;
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (first_token_received) {
                        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now - last_token_time).count();
                        if (elapsed_us >= 0) {
                            decode_times.push_back(elapsed_us / 1000000.0);
                        }
                    } else {
                        first_token_received = true;
                    }
                    last_token_time = now;
                } catch (...) {
                    // Skip non-JSON or partial chunks.
                }
            }

            // Forward chunk to client immediately
            if (!sink.write(data, length)) {
                return false; // Client disconnected
            }

            return true; // Continue streaming
        },
        {}, // Empty headers map
        timeout_seconds
    );

    if (result.status_code != 200) {
        stream_error = true;
        LOG(ERROR, "StreamingProxy") << "Backend returned error: " << result.status_code << std::endl;
    }

    if (!stream_error) {
        // Ensure [DONE] marker is sent if backend didn't send it
        if (!has_done_marker) {
            LOG(WARNING, "StreamingProxy") << "WARNING: Backend did not send [DONE] marker, adding it" << std::endl;
            const char* done_marker = "data: [DONE]\n\n";
            sink.write(done_marker, strlen(done_marker));
        }

        // Explicitly flush and signal completion
        sink.done();

        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;

        // Parse telemetry from buffered data
        auto telemetry = parse_telemetry(telemetry_buffer);
        if (!decode_times.empty()) {
            telemetry.decode_token_times = decode_times;
        }
        telemetry.print();

        if (on_complete) {
            on_complete(telemetry);
        }
    } else {
        // Properly terminate the chunked response even on error
        sink.done();
    }
}

void StreamingProxy::forward_byte_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    long timeout_seconds) {

    bool stream_error = false;

    // Use HttpClient to stream from backend
    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink](const char* data, size_t length) {
            // Forward chunk to client immediately
            if (!sink.write(data, length)) {
                return false; // Client disconnected
            }

            return true; // Continue streaming
        },
        {}, // Empty headers map
        timeout_seconds
    );

    if (result.status_code != 200) {
        stream_error = true;
        LOG(ERROR, "StreamingProxy") << "Backend returned error: " << result.status_code << std::endl;
    }

    if (!stream_error) {
        // Explicitly flush and signal completion
        sink.done();
        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;
    } else {
        // Properly terminate the chunked response even on error
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
                if (usage.contains("decode_token_times") && usage["decode_token_times"].is_array()) {
                    telemetry.decode_token_times.clear();
                    for (const auto& dt : usage["decode_token_times"]) {
                        if (dt.is_number()) {
                            telemetry.decode_token_times.push_back(dt.get<double>());
                        }
                    }
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
                if (timings.contains("predicted_ms") && timings["predicted_ms"].is_array()) {
                    telemetry.decode_token_times.clear();
                    for (const auto& ms : timings["predicted_ms"]) {
                        if (ms.is_number()) {
                            telemetry.decode_token_times.push_back(ms.get<double>() / 1000.0);
                        }
                    }
                } else if (timings.contains("predicted_per_token_ms") && timings.contains("predicted_n")) {
                    double avg_ms = timings["predicted_per_token_ms"].get<double>();
                    int num_tokens = timings["predicted_n"].get<int>();
                    if (avg_ms > 0 && num_tokens > 0) {
                        telemetry.decode_token_times.assign(static_cast<size_t>(num_tokens), avg_ms / 1000.0);
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(ERROR, "StreamingProxy") << "Error parsing telemetry: " << e.what() << std::endl;
        }
    }

    return telemetry;
}

} // namespace lemon
