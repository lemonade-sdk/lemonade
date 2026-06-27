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

// Normalize a single `data: {...}` SSE line for chat.completion.chunk objects.
// Applies two fixes for OpenAI API compliance:
//
// 1. Role normalization: some backends emit null or missing `delta.role` on
//    content chunks. Injects `"role": "assistant"` when the delta contains
//    assistant-type fields (content, reasoning_content, thinking, tool_calls,
//    function_call) but role is absent or null.
//
// 2. Content normalization: backends that emit `reasoning_content` often omit
//    the standard `content` field entirely. Injects `"content": ""` to prevent
//    OpenAI-compatible clients (e.g. @ai-sdk/openai-compatible) from resetting
//    the connection when content is expected but absent.
std::string normalize_data_line(const std::string& line) {
    const std::string prefix = "data: ";
    if (line.rfind(prefix, 0) != 0) {
        return line;
    }

    // Preserve trailing \r if present (some SSE implementations send \r\n)
    std::string suffix;
    std::string payload = line.substr(prefix.size());
    if (!payload.empty() && payload.back() == '\r') {
        suffix = "\r";
        payload.pop_back();
    }

    if (payload.empty() || payload == "[DONE]") {
        return line;
    }

    try {
        auto chunk = json::parse(payload);
        // Only normalize chat.completion.chunk objects — leave text_completion,
        // error frames, and other SSE events untouched.
        if (!chunk.is_object() ||
            !chunk.contains("object") ||
            !chunk["object"].is_string() ||
            chunk["object"].get<std::string>() != "chat.completion.chunk" ||
            !chunk.contains("choices") ||
            !chunk["choices"].is_array()) {
            return line;
        }

        bool changed = false;
        for (auto& choice : chunk["choices"]) {
            if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object()) {
                continue;
            }

            auto& delta = choice["delta"];

            // --- Fix 1: Role normalization ---
            const bool role_is_null = delta.contains("role") && delta["role"].is_null();
            const bool role_is_missing = !delta.contains("role");
            const bool has_assistant_delta =
                delta.contains("content") ||
                delta.contains("reasoning_content") ||
                delta.contains("thinking") ||
                delta.contains("tool_calls") ||
                delta.contains("function_call");

            if (role_is_null || (role_is_missing && has_assistant_delta)) {
                delta["role"] = "assistant";
                changed = true;
            }

            // --- Fix 2: Content normalization ---
            // If delta has reasoning_content but content is missing or null,
            // inject empty content string for OpenAI compatibility
            const bool has_reasoning = delta.contains("reasoning_content") &&
                                       delta["reasoning_content"].is_string();
            const bool has_content = delta.contains("content") && !delta["content"].is_null();

            if (has_reasoning && !has_content) {
                delta["content"] = "";
                changed = true;
            }
        }

        if (!changed) {
            return line;
        }

        return prefix + chunk.dump() + suffix;
    } catch (...) {
        // Malformed JSON — pass through unchanged
        return line;
    }
}

} // namespace

std::string StreamingProxy::normalize_chat_completion_chunk(const std::string& sse_chunk) {
    std::string output;
    size_t pos = 0;

    while (pos < sse_chunk.size()) {
        size_t newline = sse_chunk.find('\n', pos);
        if (newline == std::string::npos) {
            output += normalize_data_line(sse_chunk.substr(pos));
            break;
        }

        output += normalize_data_line(sse_chunk.substr(pos, newline - pos));
        output.push_back('\n');
        pos = newline + 1;
    }

    return output;
}

void StreamingProxy::forward_sse_stream(
    const std::string& backend_url,
    const std::string& request_body,
    httplib::DataSink& sink,
    std::function<void(const TelemetryData&)> on_complete,
    long timeout_seconds,
    std::function<void()> on_chunk) {

    std::string telemetry_buffer;
    bool stream_error = false;
    bool has_done_marker = false;
    bool has_first_token = false;
    double time_to_first_token = 0.0;
    const auto start_time = std::chrono::steady_clock::now();

    // Line buffer for SSE normalization: libcurl may deliver an SSE line split
    // across multiple write callbacks, so we accumulate partial input and only
    // normalize complete lines (terminated by '\n') before forwarding.
    std::string line_buffer;

    auto result = utils::HttpClient::post_stream(
        backend_url,
        request_body,
        [&sink, &telemetry_buffer, &has_done_marker, &has_first_token,
         &time_to_first_token, &start_time, &on_chunk, &line_buffer](const char* data, size_t length) {
            if (on_chunk) {
                on_chunk();
            }

            // Telemetry buffer — raw bytes, pre-normalization
            telemetry_buffer.append(data, length);

            std::string chunk(data, length);

            // First-token timing
            if (!has_first_token && chunk.find("data: ") != std::string::npos) {
                has_first_token = true;
                time_to_first_token = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_time).count();
            }

            // [DONE] marker detection
            if (chunk.find("data: [DONE]") != std::string::npos) {
                has_done_marker = true;
            }

            // Accumulate bytes and flush only complete (newline-terminated) lines
            // so normalization can safely parse each `data: {...}` payload.
            line_buffer.append(chunk);
            std::string output;
            size_t pos = 0;
            size_t newline;
            while ((newline = line_buffer.find('\n', pos)) != std::string::npos) {
                output.append(
                    StreamingProxy::normalize_chat_completion_chunk(
                        line_buffer.substr(pos, newline - pos + 1)));
                pos = newline + 1;
            }
            line_buffer.erase(0, pos);

            if (!output.empty()) {
                if (!sink.write(output.data(), output.size())) {
                    return false; // Client disconnected
                }
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
        // Flush any trailing partial line before sending [DONE]
        if (!line_buffer.empty()) {
            std::string tail = StreamingProxy::normalize_chat_completion_chunk(line_buffer);
            sink.write(tail.data(), tail.size());
            line_buffer.clear();
        }

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

        auto telemetry = parse_telemetry(telemetry_buffer);
        if (telemetry.time_to_first_token <= 0.0) {
            telemetry.time_to_first_token = time_to_first_token;
        }
        telemetry.print();

        if (on_complete) {
            on_complete(telemetry);
        }
    } else {
        sink.done();
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

} // namespace lemon