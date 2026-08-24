#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <httplib.h>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/runtime_config.h"
#include "lemon/streaming_proxy.h"
#include "lemon/utils/conversation_fingerprint.h"
#include "telemetry.h"

namespace lemon::telemetry {
    std::string standardize_thinking(const std::string& text);
    std::string hex_to_bytes(const std::string& hex);
}

static int g_failures = 0;

static void check_eq(const char* name, const std::string& actual, const std::string& expected) {
    bool ok = (actual == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("      Expected: \"%s\"\n", expected.c_str());
        std::printf("      Actual:   \"%s\"\n", actual.c_str());
        ++g_failures;
    }
}

static void check_bool(const char* name, bool actual, bool expected) {
    bool ok = (actual == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("      Expected: %s\n", expected ? "true" : "false");
        std::printf("      Actual:   %s\n", actual ? "true" : "false");
        ++g_failures;
    }
}

static void check_double(const char* name, const std::map<std::string, nlohmann::json>& m, const std::string& key, double expected) {
    auto it = m.find(key);
    bool ok = (it != m.end() && (it->second.is_number() || it->second.is_number_integer()) && std::abs(it->second.get<double>() - expected) < 1e-6);
    std::printf("[%s] %s (key: %s)\n", ok ? "PASS" : "FAIL", name, key.c_str());
    if (!ok) {
        if (it == m.end()) {
            std::printf("      Key not found in map!\n");
        } else {
            std::printf("      Expected: %f\n", expected);
            std::printf("      Actual:   %s\n", it->second.dump().c_str());
        }
        ++g_failures;
    }
}

static void check_map_empty(const char* name, const std::map<std::string, nlohmann::json>& m) {
    bool ok = m.empty();
    std::printf("[%s] %s (expected empty)\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("      Actual size: %zu\n", m.size());
        ++g_failures;
    }
}


int main() {
    using lemon::telemetry::hex_to_bytes;
    using lemon::telemetry::standardize_thinking;

    std::printf("=== RUNNING TELEMETRY HELPERS C++ TESTS ===\n");

    // --- hex_to_bytes tests ---
    check_eq("hex_to_bytes: empty string", hex_to_bytes(""), "");
    check_eq("hex_to_bytes: basic lowercase", hex_to_bytes("48656c6c6f"), "Hello");
    check_eq("hex_to_bytes: case insensitive mixed", hex_to_bytes("48656C6C6f"), "Hello");
    check_eq("hex_to_bytes: numbers only", hex_to_bytes("313233"), "123");

    // --- standardize_thinking tests ---
    check_eq("standardize_thinking: no tags", standardize_thinking("Hello world"), "Hello world");

    // Replacing start tags
    check_eq("standardize_thinking: start tag <|think|>", standardize_thinking("<|think|>hello"), "<think>hello");
    check_eq("standardize_thinking: start tag <thought>", standardize_thinking("<thought>hello"), "<think>hello");

    // Replacing end tags
    check_eq("standardize_thinking: end tag </|think|>", standardize_thinking("hello</|think|>"), "hello</think>");
    check_eq("standardize_thinking: end tag </think|>", standardize_thinking("hello</think|>"), "hello</think>");
    check_eq("standardize_thinking: end tag </thought>", standardize_thinking("hello</thought>"), "hello</think>");

    // Collapsing duplicate start tags
    check_eq("standardize_thinking: duplicate start consecutive", standardize_thinking("<think><think>hello"), "<think>hello");
    check_eq("standardize_thinking: duplicate start newline", standardize_thinking("<think>\n<think>hello"), "<think>hello");
    check_eq("standardize_thinking: duplicate start spaces", standardize_thinking("<think>  <think>hello"), "<think>hello");

    // Collapsing duplicate end tags
    check_eq("standardize_thinking: duplicate end consecutive", standardize_thinking("hello</think></think>"), "hello</think>");
    check_eq("standardize_thinking: duplicate end newline", standardize_thinking("hello</think>\n</think>"), "hello</think>");
    check_eq("standardize_thinking: duplicate end spaces", standardize_thinking("hello</think>  </think>"), "hello</think>");
    check_eq("standardize_thinking: duplicate end mixed converted", standardize_thinking("hello</think>\n</think|>"), "hello</think>");

    // Transition tags
    check_eq("standardize_thinking: transition <turn|>", standardize_thinking("<think>thinking<turn|>"), "<think>thinking</think>\n<turn|>");
    check_eq("standardize_thinking: transition <|turn>", standardize_thinking("<think>thinking<|turn>"), "<think>thinking</think>\n<|turn>");
    check_eq("standardize_thinking: transition <turn|> already closed", standardize_thinking("<think>thinking</think><turn|>"), "<think>thinking</think><turn|>");

    // Complex mixed scenario
    check_eq("standardize_thinking: complex mixed start/end/duplicate",
             standardize_thinking("<|think|><think>thinking</think>\n</think|>"),
             "<think>thinking</think>");

    check_eq("standardize_thinking: complex transition replacement",
             standardize_thinking("<thought>thinking<|turn>"),
             "<think>thinking</think>\n<|turn>");

    std::printf("===========================================\n");

    // --- parse_vllm_metrics_text tests ---
    {
        // 1. Empty body
        auto res = lemon::backends::parse_vllm_metrics_text("");
        check_map_empty("parse_vllm_metrics_text: empty body", res);
    }
    {
        // 2. Valid gauges without labels
        std::string prometheus_data =
            "# HELP vllm:gpu_cache_usage_factor GPU KV cache usage factor.\n"
            "# TYPE vllm:gpu_cache_usage_factor gauge\n"
            "vllm:gpu_cache_usage_factor 0.45\n"
            "# HELP vllm:cpu_cache_usage_factor CPU KV cache usage factor.\n"
            "vllm:cpu_cache_usage_factor 0.12\n"
            "vllm:num_requests_waiting 5\n"
            "vllm:num_requests_running 2\n"
            "vllm:num_requests_swapped 1\n";
        auto res = lemon::backends::parse_vllm_metrics_text(prometheus_data);
        check_double("parse_vllm_metrics_text: gpu_cache_usage_factor", res, "llm.vllm.gpu_cache_usage_factor", 0.45);
        check_double("parse_vllm_metrics_text: cpu_cache_usage_factor", res, "llm.vllm.cpu_cache_usage_factor", 0.12);
        check_double("parse_vllm_metrics_text: num_requests_waiting", res, "llm.vllm.num_requests_waiting", 5.0);
        check_double("parse_vllm_metrics_text: num_requests_running", res, "llm.vllm.num_requests_running", 2.0);
        check_double("parse_vllm_metrics_text: num_requests_swapped", res, "llm.vllm.num_requests_swapped", 1.0);
    }
    {
        // 3. Gauges with labels
        std::string prometheus_data =
            "vllm:gpu_cache_usage_factor{model=\"some_model\"} 0.75\n"
            "vllm:num_requests_waiting{model=\"some_model\"} 12\n";
        auto res = lemon::backends::parse_vllm_metrics_text(prometheus_data);
        check_double("parse_vllm_metrics_text labeled: gpu_cache_usage_factor", res, "llm.vllm.gpu_cache_usage_factor", 0.75);
        check_double("parse_vllm_metrics_text labeled: num_requests_waiting", res, "llm.vllm.num_requests_waiting", 12.0);
    }
    {
        // 4. Malformed input
        std::string prometheus_data =
            "vllm:gpu_cache_usage_factor\n"
            "vllm:num_requests_waiting abc\n"
            "vllm:num_requests_running 1.5 2.5\n";
        auto res = lemon::backends::parse_vllm_metrics_text(prometheus_data);
        check_double("parse_vllm_metrics_text malformed: num_requests_running last space", res, "llm.vllm.num_requests_running", 2.5);

        auto it_gpu = res.find("llm.vllm.gpu_cache_usage_factor");
        bool gpu_missing = (it_gpu == res.end());
        std::printf("[%s] parse_vllm_metrics_text malformed: gpu_cache_usage_factor is skipped\n", gpu_missing ? "PASS" : "FAIL");
        if (!gpu_missing) ++g_failures;

        auto it_wait = res.find("llm.vllm.num_requests_waiting");
        bool wait_missing = (it_wait == res.end());
        std::printf("[%s] parse_vllm_metrics_text malformed: num_requests_waiting (abc) is skipped\n", wait_missing ? "PASS" : "FAIL");
        if (!wait_missing) ++g_failures;
    }

    // --- parse_telemetry tests ---
    std::printf("===========================================\n");
    {
        auto check_int = [](const char* name, int actual, int expected) {
            bool ok = (actual == expected);
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) {
                std::printf("      Expected: %d\n", expected);
                std::printf("      Actual:   %d\n", actual);
                ++g_failures;
            }
        };

        auto check_double_val = [](const char* name, double actual, double expected) {
            bool ok = (std::abs(actual - expected) < 1e-6);
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) {
                std::printf("      Expected: %f\n", expected);
                std::printf("      Actual:   %f\n", actual);
                ++g_failures;
            }
        };

        // 1. Root level usage
        {
            std::string buffer = "data: {\"usage\": {\"prompt_tokens\": 10, \"completion_tokens\": 20}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: root usage prompt_tokens", tel.input_tokens, 10);
            check_int("parse_telemetry: root usage completion_tokens", tel.output_tokens, 20);
            check_int("parse_telemetry: usage sets prompt_tokens field", tel.prompt_tokens, 10);
            check_int("parse_telemetry: cache_tokens -1 when unreported", tel.cache_tokens, -1);
        }

        // 1a. extract_telemetry on complete response bodies (non-streaming)
        {
            nlohmann::json chat_body = nlohmann::json::parse(
                "{\"usage\": {\"prompt_tokens\": 59, \"completion_tokens\": 3, "
                "\"prompt_tokens_details\": {\"cached_tokens\": 33}}, "
                "\"timings\": {\"prompt_n\": 19, \"predicted_n\": 3, \"prompt_ms\": 100.0, "
                "\"predicted_per_second\": 50.0, \"cache_n\": 40}}");
            auto tel = lemon::StreamingProxy::extract_telemetry(chat_body);
            check_int("extract_telemetry: timings override input", tel.input_tokens, 19);
            check_int("extract_telemetry: usage prompt preserved", tel.prompt_tokens, 59);
            check_int("extract_telemetry: timings cache_n wins", tel.cache_tokens, 40);
            check_int("extract_telemetry: output from timings", tel.output_tokens, 3);
        }
        {
            nlohmann::json responses_body = nlohmann::json::parse(
                "{\"usage\": {\"input_tokens\": 30, \"output_tokens\": 40, "
                "\"input_tokens_details\": {\"cached_tokens\": 12}}}");
            auto tel = lemon::StreamingProxy::extract_telemetry(responses_body);
            check_int("extract_telemetry: responses input_tokens", tel.input_tokens, 30);
            check_int("extract_telemetry: responses prompt from input", tel.prompt_tokens, 30);
            check_int("extract_telemetry: responses cached via input_tokens_details", tel.cache_tokens, 12);
            check_int("extract_telemetry: responses output_tokens", tel.output_tokens, 40);
        }

        // 1b. Cached tokens from usage.prompt_tokens_details (OpenAI-wire)
        {
            std::string buffer = "data: {\"usage\": {\"prompt_tokens\": 10, \"completion_tokens\": 20, \"prompt_tokens_details\": {\"cached_tokens\": 8}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: usage cached_tokens", tel.cache_tokens, 8);
        }
        {
            std::string buffer = "data: {\"usage\": {\"prompt_tokens\": 10, \"prompt_tokens_details\": {\"cached_tokens\": 0}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: usage cached_tokens zero is reported, not -1", tel.cache_tokens, 0);
        }

        // 1c. Responses API usage shape: input_tokens_details.cached_tokens
        {
            std::string buffer = "data: {\"response\": {\"usage\": {\"input_tokens\": 30, \"output_tokens\": 40, \"input_tokens_details\": {\"cached_tokens\": 12}}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: responses input_tokens_details cached_tokens", tel.cache_tokens, 12);
            check_int("parse_telemetry: responses input_tokens alongside details", tel.input_tokens, 30);
        }
        {
            std::string buffer = "data: {\"usage\": {\"input_tokens\": 30, \"input_tokens_details\": {\"cached_tokens\": 7}, \"prompt_tokens_details\": {\"cached_tokens\": 9}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: prompt_tokens_details wins over input_tokens_details", tel.cache_tokens, 9);
        }

        // 2. Nested usage under response (OpenAI keys)
        {
            std::string buffer = "data: {\"response\": {\"usage\": {\"prompt_tokens\": 30, \"completion_tokens\": 40, \"prefill_duration_ttft\": 0.15, \"decoding_speed_tps\": 45.2}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: nested usage prompt_tokens", tel.input_tokens, 30);
            check_int("parse_telemetry: nested usage completion_tokens", tel.output_tokens, 40);
            check_double_val("parse_telemetry: nested usage prefill_duration_ttft", tel.time_to_first_token, 0.15);
            check_double_val("parse_telemetry: nested usage decoding_speed_tps", tel.tokens_per_second, 45.2);
        }

        // 2b. Nested usage under response (Responses API keys)
        {
            std::string buffer = "data: {\"response\": {\"usage\": {\"input_tokens\": 35, \"output_tokens\": 45, \"prefill_duration_ttft\": 0.18, \"decoding_speed_tps\": 50.0}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: nested usage input_tokens", tel.input_tokens, 35);
            check_int("parse_telemetry: nested usage output_tokens", tel.output_tokens, 45);
            check_double_val("parse_telemetry: nested usage input prefill_duration_ttft", tel.time_to_first_token, 0.18);
            check_double_val("parse_telemetry: nested usage input decoding_speed_tps", tel.tokens_per_second, 50.0);
        }

        // 3. Root level timings
        {
            std::string buffer = "data: {\"timings\": {\"prompt_n\": 50, \"predicted_n\": 60, \"prompt_ms\": 1500.0, \"predicted_per_second\": 25.5, \"cache_n\": 45}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: root timings prompt_tokens", tel.input_tokens, 50);
            check_int("parse_telemetry: root timings completion_tokens", tel.output_tokens, 60);
            check_double_val("parse_telemetry: root timings prompt_ms", tel.time_to_first_token, 1.5);
            check_double_val("parse_telemetry: root timings predicted_per_second", tel.tokens_per_second, 25.5);
            check_int("parse_telemetry: root timings cache_n", tel.cache_tokens, 45);
        }

        // 4. Nested timings under response
        {
            std::string buffer = "data: {\"response\": {\"timings\": {\"prompt_n\": 70, \"predicted_n\": 80, \"prompt_ms\": 2000.0, \"predicted_per_second\": 30.0}}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: nested timings prompt_tokens", tel.input_tokens, 70);
            check_int("parse_telemetry: nested timings completion_tokens", tel.output_tokens, 80);
            check_double_val("parse_telemetry: nested timings prompt_ms", tel.time_to_first_token, 2.0);
            check_double_val("parse_telemetry: nested timings predicted_per_second", tel.tokens_per_second, 30.0);
        }
    }

    // --- conversation_fingerprint tests ---
    std::printf("===========================================\n");
    {
        auto check_bool = [](const char* name, bool ok) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++g_failures;
        };
        using lemon::utils::conversation_fingerprint;

        nlohmann::json turn1 = {{"messages", nlohmann::json::array({
            {{"role", "system"}, {"content", "You are helpful."}},
            {{"role", "user"}, {"content", "Refactor my parser"}}
        })}};
        nlohmann::json turn2 = {{"messages", nlohmann::json::array({
            {{"role", "system"}, {"content", "You are helpful."}},
            {{"role", "user"}, {"content", "Refactor my parser"}},
            {{"role", "assistant"}, {"content", "Done."}},
            {{"role", "user"}, {"content", "now add tests"}}
        })}};
        check_bool("conversation_fingerprint: stable across appended turns",
                   conversation_fingerprint(turn1) == conversation_fingerprint(turn2));

        nlohmann::json other = {{"messages", nlohmann::json::array({
            {{"role", "system"}, {"content", "You are helpful."}},
            {{"role", "user"}, {"content", "Write a poem"}}
        })}};
        check_bool("conversation_fingerprint: differs for different first user message",
                   conversation_fingerprint(turn1) != conversation_fingerprint(other));

        nlohmann::json no_system = {{"messages", nlohmann::json::array({
            {{"role", "user"}, {"content", "Refactor my parser"}}
        })}};
        check_bool("conversation_fingerprint: system prompt participates",
                   conversation_fingerprint(turn1) != conversation_fingerprint(no_system));

        nlohmann::json parts = {{"messages", nlohmann::json::array({
            {{"role", "system"}, {"content", "You are helpful."}},
            {{"role", "user"}, {"content", nlohmann::json::array({
                {{"type", "text"}, {"text", "Refactor my parser"}}
            })}}
        })}};
        check_bool("conversation_fingerprint: content-part form matches plain string form",
                   conversation_fingerprint(turn1) == conversation_fingerprint(parts));

        nlohmann::json prompt_a = {{"prompt", "complete this"}};
        nlohmann::json prompt_b = {{"prompt", "complete that"}};
        check_bool("conversation_fingerprint: legacy prompt form distinguishes inputs",
                   conversation_fingerprint(prompt_a) != conversation_fingerprint(prompt_b));
    }

    // --- accumulate_responses_delta tests ---
    std::printf("===========================================\n");
    {
        // a) Verify that chunk type "response.output_text.delta" with string delta contributes to accumulated_text.
        {
            std::string acc = "";
            nlohmann::json chunk = {
                {"type", "response.output_text.delta"},
                {"delta", "hello"}
            };
            lemon::StreamingProxy::accumulate_responses_delta(chunk, acc);
            check_eq("accumulate_responses_delta: type output_text.delta + string delta", acc, "hello");
        }

        // b) Verify that chunk type "response.output_text.delta" with object delta (text field) contributes to accumulated_text.
        {
            std::string acc = "";
            nlohmann::json chunk = {
                {"type", "response.output_text.delta"},
                {"delta", {{"text", " world"}}}
            };
            lemon::StreamingProxy::accumulate_responses_delta(chunk, acc);
            check_eq("accumulate_responses_delta: type output_text.delta + object delta", acc, " world");
        }

        // c) Verify that a non-text event (e.g., type == "response.audio.delta" or type == "response.tool.delta") does NOT contribute to accumulated_text.
        {
            std::string acc = "";
            nlohmann::json audio_chunk = {
                {"type", "response.audio.delta"},
                {"delta", "audio_data"}
            };
            lemon::StreamingProxy::accumulate_responses_delta(audio_chunk, acc);
            check_eq("accumulate_responses_delta: non-text type response.audio.delta (string)", acc, "");

            nlohmann::json tool_chunk = {
                {"type", "response.tool.delta"},
                {"delta", {{"text", "tool_data"}}}
            };
            lemon::StreamingProxy::accumulate_responses_delta(tool_chunk, acc);
            check_eq("accumulate_responses_delta: non-text type response.tool.delta (object)", acc, "");
        }

        // d) Verify that chunks without 'type' (fallback/OpenAI chat completion chunks) still contribute to accumulated_text.
        {
            std::string acc = "";
            nlohmann::json chunk_no_type_string = {
                {"delta", "fallback string"}
            };
            lemon::StreamingProxy::accumulate_responses_delta(chunk_no_type_string, acc);
            check_eq("accumulate_responses_delta: no type + string delta", acc, "fallback string");

            acc = "";
            nlohmann::json chunk_no_type_obj = {
                {"delta", {{"text", "fallback object"}}}
            };
            lemon::StreamingProxy::accumulate_responses_delta(chunk_no_type_obj, acc);
            check_eq("accumulate_responses_delta: no type + object delta", acc, "fallback object");

            acc = "";
            nlohmann::json chunk_openai = {
                {"choices", nlohmann::json::array({{{"delta", {{"content", "OpenAI content"}}}}})}
            };
            lemon::StreamingProxy::accumulate_responses_delta(chunk_openai, acc);
            check_eq("accumulate_responses_delta: no type + choices array delta", acc, "OpenAI content");
        }
    }

    // --- Client disconnect telemetry error handling tests ---
    std::printf("===========================================\n");
    {
#ifdef _WIN32
        _putenv("NO_PROXY=*");
        _putenv("no_proxy=*");
#else
        setenv("NO_PROXY", "*", 1);
        setenv("no_proxy", "*", 1);
#endif
        httplib::Server svr;
        svr.Post("/stream", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content_provider(
                "text/event-stream",
                [](size_t offset, httplib::DataSink& sink) {
                    sink.write("data: hello\n\n", 13);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    sink.write("data: [DONE]\n\n", 14);
                    sink.done();
                    return true;
                }
            );
        });

        int port = svr.bind_to_any_port("127.0.0.1");
        if (port < 0) {
            std::printf("[FAIL] Failed to bind httplib::Server to any port\n");
            ++g_failures;
        } else {
            std::thread server_thread([&svr]() {
                svr.listen_after_bind();
            });
            svr.wait_until_ready();

            httplib::DataSink sink;
            sink.write = [](const char* data, size_t len) {
                // Abort the stream by returning false
                return false;
            };
            sink.done = []() {};

            bool callback_called = false;
            std::string error_msg = "";

            std::string backend_url = "http://127.0.0.1:" + std::to_string(port) + "/stream";

            lemon::StreamingProxy::forward_sse_stream(
                backend_url,
                "{}",
                sink,
                [&callback_called, &error_msg](const lemon::StreamingProxy::TelemetryData& tel) {
                    callback_called = true;
                    error_msg = tel.error_message;
                },
                5 // 5 seconds timeout
            );

            svr.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }

            if (callback_called) {
                std::printf("[PASS] forward_sse_stream abort: callback was called\n");
            } else {
                std::printf("[FAIL] forward_sse_stream abort: callback was NOT called\n");
                ++g_failures;
            }

            check_eq("Client disconnected error message check", error_msg, "Client disconnected during stream");
        }
    }

    // --- InferenceSpan session ID resolution tests ---
    std::printf("===========================================\n");
    {
        nlohmann::json test_cfg_json = {
            {"config_version", 2},
            {"port", 13305},
            {"host", "localhost"},
            {"telemetry", {
                {"enabled", false},
                {"max_attribute_length", 20},
                {"otlp", {
                    {"semantics", {"openinference", "otel_genai"}}
                }}
            }}
        };
        lemon::RuntimeConfig test_cfg(test_cfg_json);
        lemon::RuntimeConfig::set_global(&test_cfg);

        auto get_span_attr = [](const nlohmann::json& span, const std::string& attr_key) -> std::string {
            if (span.contains("attributes") && span["attributes"].is_array()) {
                for (const auto& attr : span["attributes"]) {
                    if (attr.value("key", "") == attr_key) {
                        if (attr.contains("value") && attr["value"].contains("stringValue")) {
                            return attr["value"]["stringValue"].get<std::string>();
                        }
                    }
                }
            }
            return "";
        };

        nlohmann::json last_span;
        lemon::telemetry::register_span_listener([&last_span](const nlohmann::json& span) {
            last_span = span;
        });

        // 1. Body session_id present, header present -> body wins
        lemon::telemetry::g_incoming_client_id = "hdr_client";
        lemon::telemetry::g_incoming_session_id = "hdr_session_123";
        auto span1 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", {{"session_id", "body_session_456"}});
        span1->end_with_success(nlohmann::json::object(), "response text");
        check_eq("InferenceSpan session: body wins over header (session.id)", get_span_attr(last_span, "session.id"), "body_session_456");
        check_eq("InferenceSpan session: body wins over header (gen_ai)", get_span_attr(last_span, "gen_ai.conversation.id"), "body_session_456");

        // 2. Body session_id explicitly empty, header present -> body wins (empty session.id, no header fallback)
        auto span1_empty = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", {{"session_id", ""}});
        span1_empty->end_with_success(nlohmann::json::object(), "response text");
        check_eq("InferenceSpan session: explicit empty body wins over header (session.id)", get_span_attr(last_span, "session.id"), "");
        check_eq("InferenceSpan session: explicit empty body wins over header (gen_ai)", get_span_attr(last_span, "gen_ai.conversation.id"), "");

        // 3. Body session_id absent, header present -> header used
        lemon::telemetry::g_incoming_client_id = "";
        lemon::telemetry::g_incoming_session_id = "hdr_session_789";
        auto span2 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span2->end_with_success(nlohmann::json::object(), "response text");
        check_eq("InferenceSpan session: header fallback (session.id)", get_span_attr(last_span, "session.id"), "hdr_session_789");
        check_eq("InferenceSpan session: header fallback (gen_ai)", get_span_attr(last_span, "gen_ai.conversation.id"), "hdr_session_789");

        // 4. Namespaced header session (<client>/<session>) with long client preserves session ID
        lemon::telemetry::g_incoming_client_id = "opencode-cli-desktop-extra-long";
        lemon::telemetry::g_incoming_session_id = "sess-xyz";
        auto span3 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span3->end_with_success(nlohmann::json::object(), "response text");
        check_eq("InferenceSpan session: namespaced preserves session ID (session.id)", get_span_attr(last_span, "session.id"), "opencode-cl/sess-xyz");
        check_eq("InferenceSpan session: namespaced preserves session ID (gen_ai)", get_span_attr(last_span, "gen_ai.conversation.id"), "opencode-cl/sess-xyz");

        // 5. Neither body nor header present -> omitted from attributes
        lemon::telemetry::g_incoming_client_id = "";
        lemon::telemetry::g_incoming_session_id = "";
        auto span4 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span4->end_with_success(nlohmann::json::object(), "response text");
        check_eq("InferenceSpan session: omitted when absent (session.id)", get_span_attr(last_span, "session.id"), "");
        check_eq("InferenceSpan session: omitted when absent (gen_ai)", get_span_attr(last_span, "gen_ai.conversation.id"), "");

        // 6. Tool calls output messages, max_attribute_length truncation with valid JSON, and fallback output.value
        std::vector<lemon::telemetry::ToolCall> sample_tool_calls = {
            {"call_abc123", "bash", "{\"command\": \"git branch --all -vv\"}"}
        };
        auto span5 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span5->end_with_success(nlohmann::json::object(), "", sample_tool_calls);
        check_eq("InferenceSpan tool calls: openinference role", get_span_attr(last_span, "llm.output_messages.0.message.role"), "assistant");
        check_eq("InferenceSpan tool calls: openinference tool id", get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.id"), "call_abc123");
        check_eq("InferenceSpan tool calls: openinference function name", get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.function.name"), "bash");
        std::string args_str = get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.function.arguments");
        nlohmann::json parsed_args = nlohmann::json::parse(args_str, nullptr, false);
        check_bool("InferenceSpan tool calls: function args is valid JSON", parsed_args.is_discarded(), false);
        check_bool("InferenceSpan tool calls: function args has _truncated flag", parsed_args.value("_truncated", false), true);
        check_eq("InferenceSpan tool calls: openinference fallback output.value truncated", get_span_attr(last_span, "output.value"), "[{\"fu... [TRUNCATED]");

        // 7. hide_thinking preserves stripped output in output_messages content
        nlohmann::json test_cfg_thinking_json = {
            {"config_version", 2},
            {"port", 13305},
            {"host", "localhost"},
            {"telemetry", {
                {"enabled", false},
                {"hide_outputs", false},
                {"hide_thinking", true},
                {"otlp", {
                    {"semantics", {"openinference", "otel_genai"}}
                }}
            }}
        };
        lemon::RuntimeConfig test_cfg_thinking(test_cfg_thinking_json);
        lemon::RuntimeConfig::set_global(&test_cfg_thinking);

        auto span6 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span6->end_with_success(nlohmann::json::object(), "<think>internal reasoning</think>visible reply");
        check_eq("InferenceSpan hide_thinking: openinference content stripped", get_span_attr(last_span, "llm.output_messages.0.message.content"), "visible reply");
        check_eq("InferenceSpan hide_thinking: gen_ai content stripped", get_span_attr(last_span, "gen_ai.output.messages.0.content"), "visible reply");

        // 8. hide_outputs suppresses tool-call attributes and redacts output content
        nlohmann::json test_cfg_hide_out_json = {
            {"config_version", 2},
            {"port", 13305},
            {"host", "localhost"},
            {"telemetry", {
                {"enabled", false},
                {"hide_outputs", true},
                {"otlp", {
                    {"semantics", {"openinference", "otel_genai"}}
                }}
            }}
        };
        lemon::RuntimeConfig test_cfg_hide_out(test_cfg_hide_out_json);
        lemon::RuntimeConfig::set_global(&test_cfg_hide_out);

        auto span7 = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
        span7->end_with_success(nlohmann::json::object(), "sensitive text", sample_tool_calls);
        check_eq("InferenceSpan hide_outputs: output.value redacted", get_span_attr(last_span, "output.value"), "[REDACTED]");
        check_eq("InferenceSpan hide_outputs: openinference content redacted", get_span_attr(last_span, "llm.output_messages.0.message.content"), "[REDACTED]");
        check_eq("InferenceSpan hide_outputs: openinference tool id suppressed", get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.id"), "");
        check_eq("InferenceSpan hide_outputs: openinference function name suppressed", get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.function.name"), "");
        // 9. Unicode boundary and strict max_attribute_length bounds
        std::vector<lemon::telemetry::ToolCall> unicode_tool_calls = {
            {"call_uni", "search", "{\"query\": \"日本語テスト🚀 emoji and characters\"}"}
        };
        for (int bound : {0, 1, 2, 10, 18, 30, 50, 100}) {
            nlohmann::json test_cfg_bound_json = {
                {"config_version", 2},
                {"port", 13305},
                {"host", "localhost"},
                {"telemetry", {
                    {"enabled", false},
                    {"max_attribute_length", bound},
                    {"otlp", {
                        {"semantics", {"openinference"}}
                    }}
                }}
            };
            lemon::RuntimeConfig test_cfg_bound(test_cfg_bound_json);
            lemon::RuntimeConfig::set_global(&test_cfg_bound);

            auto span_bound = lemon::telemetry::TelemetryTracker::start_span("LLM", "chat.completions", "test-model", nlohmann::json::object());
            span_bound->end_with_success(nlohmann::json::object(), "", unicode_tool_calls);
            std::string bound_args = get_span_attr(last_span, "llm.output_messages.0.message.tool_calls.0.tool_call.function.arguments");
            check_bool("InferenceSpan unicode args length <= max_len", bound_args.size() <= static_cast<size_t>(bound), true);
            if (!bound_args.empty()) {
                nlohmann::json p = nlohmann::json::parse(bound_args, nullptr, false);
                check_bool("InferenceSpan unicode args valid JSON", p.is_discarded(), false);
            }
            lemon::RuntimeConfig::set_global(nullptr);
        }

        // Clean up
        lemon::telemetry::unregister_span_listener();
        lemon::RuntimeConfig::set_global(nullptr);
        lemon::telemetry::flush();
        lemon::telemetry::shutdown();
    }

    std::printf("===========================================\n");
    if (g_failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", g_failures);
        return 1;
    } else {
        std::printf("All C++ telemetry helper tests PASSED.\n");
        return 0;
    }
}
