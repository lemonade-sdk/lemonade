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
#include "lemon/streaming_proxy.h"

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
            std::string buffer = "data: {\"timings\": {\"prompt_n\": 50, \"predicted_n\": 60, \"prompt_ms\": 1500.0, \"predicted_per_second\": 25.5}}\n";
            auto tel = lemon::StreamingProxy::parse_telemetry(buffer);
            check_int("parse_telemetry: root timings prompt_tokens", tel.input_tokens, 50);
            check_int("parse_telemetry: root timings completion_tokens", tel.output_tokens, 60);
            check_double_val("parse_telemetry: root timings prompt_ms", tel.time_to_first_token, 1.5);
            check_double_val("parse_telemetry: root timings predicted_per_second", tel.tokens_per_second, 25.5);
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

    std::printf("===========================================\n");
    if (g_failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", g_failures);
        return 1;
    } else {
        std::printf("All C++ telemetry helper tests PASSED.\n");
        return 0;
    }
}
