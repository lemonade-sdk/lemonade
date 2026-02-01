#include "lemon/streaming_proxy.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <vector>

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

    // Track decode times by measuring intervals between token chunks
    std::vector<double> decode_times;
    std::chrono::steady_clock::time_point last_token_time;
    bool first_token_received = false;
    int token_count = 0;

    // Use HttpClient to stream from backend
    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &telemetry_buffer, &has_done_marker, &decode_times,
            &last_token_time, &first_token_received, &token_count]
            (const char* data, size_t length) {
            // Buffer for telemetry parsing
            telemetry_buffer.append(data, length);

            // Check if this chunk contains [DONE]
            std::string chunk(data, length);
            if (chunk.find("[DONE]") != std::string::npos) {
                has_done_marker = true;
            }
            // Parse chunks to detect token content and track timing
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("data: ") == 0) {
                    std::string json_str = line.substr(6);
                    if (json_str != "[DONE]" && !json_str.empty()) {
                        try {
                            auto chunk_json = json::parse(json_str);
                            // Check if this chunk contains token content
                            if (chunk_json.contains("choices") && chunk_json["choices"].is_array() &&
                                !chunk_json["choices"].empty()) {
                                auto choice = chunk_json["choices"][0];
                                if (choice.contains("delta")) {
                                    auto delta = choice["delta"];
                                    // Check for content, reasoning_content, or any token data
                                    bool has_content = (delta.contains("content") && !delta["content"].is_null() &&
                                                       !delta["content"].get<std::string>().empty()) ||
                                                      (delta.contains("reasoning_content") &&
                                                       !delta["reasoning_content"].is_null() &&
                                                       !delta["reasoning_content"].get<std::string>().empty());
                                    if (has_content) {
                                        auto now = std::chrono::steady_clock::now();
                                        if (first_token_received) {
                                            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                                now - last_token_time).count() / 1000000.0;
                                            decode_times.push_back(elapsed);
                                        } else {
                                            first_token_received = true;
                                        }
                                        last_token_time = now;
                                        token_count++;
                                    }
                                }
                            }
                        } catch (...) {
                            // Skip invalid JSON
                        }
                    }
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
        std::cerr << "[StreamingProxy] Backend returned error: " << result.status_code << std::endl;
    }

    if (!stream_error) {
        // Ensure [DONE] marker is sent if backend didn't send it
        if (!has_done_marker) {
            std::cerr << "[StreamingProxy] WARNING: Backend did not send [DONE] marker, adding it" << std::endl;
            const char* done_marker = "data: [DONE]\n\n";
            sink.write(done_marker, strlen(done_marker));
        }

        // Explicitly flush and signal completion
        sink.done();

        std::cout << "[Server] Streaming completed - 200 OK" << std::endl;

        // Parse telemetry from buffered data
        auto telemetry = parse_telemetry(telemetry_buffer);
        // Use tracked decode times if available (more accurate than backend aggregate)
        if (!decode_times.empty()) {
            telemetry.decode_token_times = decode_times;
        }
        telemetry.print();

        if (on_complete) {
            on_complete(telemetry);
        }
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
                // Extract decode_token_times if available in timings
                // llama.cpp may expose predicted_ms as array of per-token times
                if (timings.contains("predicted_ms")) {
                    if (timings["predicted_ms"].is_array()) {
                        // Array of per-token times
                        auto predicted_ms = timings["predicted_ms"];
                        telemetry.decode_token_times.clear();
                        for (const auto& ms : predicted_ms) {
                            if (ms.is_number()) {
                                telemetry.decode_token_times.push_back(ms.get<double>() / 1000.0);
                            }
                        }
                    } else if (timings.contains("predicted_per_token_ms") &&
                              timings.contains("predicted_n")) {
                        // Backend only provides average - distribute evenly
                        // This is less accurate but better than nothing
                        double avg_ms = timings["predicted_per_token_ms"].get<double>();
                        int num_tokens = timings["predicted_n"].get<int>();
                        if (num_tokens > 0 && avg_ms > 0) {
                            telemetry.decode_token_times.clear();
                            double avg_seconds = avg_ms / 1000.0;
                            for (int i = 0; i < num_tokens; i++) {
                                telemetry.decode_token_times.push_back(avg_seconds);
                            }
                        }
                    }
                }
            }
            // Check for decode_token_times in usage object (some backends may expose it there)
            if (last_chunk_with_usage.contains("usage")) {
                auto usage = last_chunk_with_usage["usage"];
                if (usage.contains("decode_token_times") && usage["decode_token_times"].is_array()) {
                    auto decode_times = usage["decode_token_times"];
                    telemetry.decode_token_times.clear();
                    for (const auto& dt : decode_times) {
                        if (dt.is_number()) {
                            telemetry.decode_token_times.push_back(dt.get<double>());
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[StreamingProxy] Error parsing telemetry: " << e.what() << std::endl;
        }
    }

    return telemetry;
}

} // namespace lemon
