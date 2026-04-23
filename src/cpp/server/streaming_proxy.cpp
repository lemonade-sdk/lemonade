#include "lemon/streaming_proxy.h"
#include <sstream>
#include <iostream>
#include <chrono>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

void StreamingProxy::forward_sse_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds) {

    std::string telemetry_buffer;
    const auto request_start = std::chrono::steady_clock::now();
    bool saw_first_chunk = false;
    std::chrono::steady_clock::time_point first_chunk_at;
    bool stream_error = false;
    bool has_done_marker = false;

    // Use HttpClient to stream from backend
    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &telemetry_buffer, &has_done_marker, &saw_first_chunk, &first_chunk_at](const char* data, size_t length) {
            // Buffer for telemetry parsing
            telemetry_buffer.append(data, length);

            if (!saw_first_chunk && length > 0) {
                saw_first_chunk = true;
                first_chunk_at = std::chrono::steady_clock::now();
            }

            // Check if this chunk contains [DONE]
            std::string chunk(data, length);
            if (chunk.find("[DONE]") != std::string::npos) {
                has_done_marker = true;
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

        // Some SSE protocols (e.g., Responses API) report usage but omit timing fields.
        const auto finished_at = std::chrono::steady_clock::now();
        if (saw_first_chunk && telemetry.time_to_first_token <= 0.0) {
            telemetry.time_to_first_token =
                std::chrono::duration<double>(first_chunk_at - request_start).count();
        }
        if (saw_first_chunk && telemetry.tokens_per_second <= 0.0 && telemetry.output_tokens > 0) {
            const double decode_seconds =
                std::chrono::duration<double>(finished_at - first_chunk_at).count();
            if (decode_seconds > 0.0) {
                telemetry.tokens_per_second =
                    static_cast<double>(telemetry.output_tokens) / decode_seconds;
            }
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
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds) {

    std::string telemetry_buffer;
    const auto request_start = std::chrono::steady_clock::now();
    bool saw_first_chunk = false;
    std::chrono::steady_clock::time_point first_chunk_at;
    bool stream_error = false;

    // Use HttpClient to stream from backend
    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &telemetry_buffer, &saw_first_chunk, &first_chunk_at](const char* data, size_t length) {
            telemetry_buffer.append(data, length);

            if (!saw_first_chunk && length > 0) {
                saw_first_chunk = true;
                first_chunk_at = std::chrono::steady_clock::now();
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
        // Explicitly flush and signal completion
        sink.done();
        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;

        auto telemetry = parse_telemetry(telemetry_buffer);

        // Anthropic streams often omit timing fields. Derive TTFT/TPS from wall clock if needed.
        const auto finished_at = std::chrono::steady_clock::now();
        if (saw_first_chunk && telemetry.time_to_first_token <= 0.0) {
            telemetry.time_to_first_token =
                std::chrono::duration<double>(first_chunk_at - request_start).count();
        }
        if (saw_first_chunk && telemetry.tokens_per_second <= 0.0 && telemetry.output_tokens > 0) {
            const double decode_seconds =
                std::chrono::duration<double>(finished_at - first_chunk_at).count();
            if (decode_seconds > 0.0) {
                telemetry.tokens_per_second =
                    static_cast<double>(telemetry.output_tokens) / decode_seconds;
            }
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

StreamingProxy::TelemetryData StreamingProxy::parse_telemetry(const std::string& buffer) {
    TelemetryData telemetry;

    std::istringstream stream(buffer);
    std::string line;

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
                if (chunk.contains("usage") && chunk["usage"].is_object()) {
                    auto usage = chunk["usage"];

                    if (usage.contains("prompt_tokens")) {
                        telemetry.input_tokens = usage["prompt_tokens"].get<int>();
                    }
                    if (usage.contains("completion_tokens")) {
                        telemetry.output_tokens = usage["completion_tokens"].get<int>();
                    }

                    // Anthropic native usage fields
                    if (usage.contains("input_tokens")) {
                        telemetry.input_tokens = usage["input_tokens"].get<int>();
                    }
                    if (usage.contains("output_tokens")) {
                        telemetry.output_tokens = usage["output_tokens"].get<int>();
                    }

                    // FLM format
                    if (usage.contains("prefill_duration_ttft")) {
                        telemetry.time_to_first_token = usage["prefill_duration_ttft"].get<double>();
                    }
                    if (usage.contains("decoding_speed_tps")) {
                        telemetry.tokens_per_second = usage["decoding_speed_tps"].get<double>();
                    }
                }

                // Anthropic streaming message_start embeds usage under message.usage.
                if (chunk.contains("message") && chunk["message"].is_object()) {
                    const auto& message = chunk["message"];
                    if (message.contains("usage") && message["usage"].is_object()) {
                        const auto& usage = message["usage"];
                        if (usage.contains("input_tokens")) {
                            telemetry.input_tokens = usage["input_tokens"].get<int>();
                        }
                        if (usage.contains("output_tokens")) {
                            telemetry.output_tokens = usage["output_tokens"].get<int>();
                        }
                    }
                }

                if (chunk.contains("timings") && chunk["timings"].is_object()) {
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

                // OpenAI Responses streaming often reports usage under response.usage
                // in response.completed events.
                if (chunk.contains("response") && chunk["response"].is_object()) {
                    const auto& response = chunk["response"];
                    if (response.contains("usage") && response["usage"].is_object()) {
                        const auto& usage = response["usage"];
                        if (usage.contains("prompt_tokens")) {
                            telemetry.input_tokens = usage["prompt_tokens"].get<int>();
                        }
                        if (usage.contains("completion_tokens")) {
                            telemetry.output_tokens = usage["completion_tokens"].get<int>();
                        }
                        if (usage.contains("input_tokens")) {
                            telemetry.input_tokens = usage["input_tokens"].get<int>();
                        }
                        if (usage.contains("output_tokens")) {
                            telemetry.output_tokens = usage["output_tokens"].get<int>();
                        }
                        if (usage.contains("prefill_duration_ttft")) {
                            telemetry.time_to_first_token = usage["prefill_duration_ttft"].get<double>();
                        }
                        if (usage.contains("decoding_speed_tps")) {
                            telemetry.tokens_per_second = usage["decoding_speed_tps"].get<double>();
                        }
                    }
                }
            } catch (const std::exception&) {
                // Skip invalid JSON
            }
        }
    }

    return telemetry;
}

} // namespace lemon
