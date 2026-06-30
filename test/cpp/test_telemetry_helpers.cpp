#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include "lemon/backends/vllm/vllm_server.h"

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

    std::printf("===========================================\n");
    if (g_failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", g_failures);
        return 1;
    } else {
        std::printf("All C++ telemetry helper tests PASSED.\n");
        return 0;
    }
}
