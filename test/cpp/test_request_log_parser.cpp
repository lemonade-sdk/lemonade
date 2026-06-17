// Unit tests for request log parser helpers.

#include "lemon/request_log_parser.h"

#include <cassert>
#include <cstdio>
#include <string>

using lemon::ParsedRequestBody;
using lemon::classify_endpoint_type;
using lemon::parse_request_body;
using lemon::redact_json;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

static void test_endpoint_classification(TestResult& result) {
    if (classify_endpoint_type("/api/chat", "POST") == "ollama") {
        result.ok("ollama chat path");
    } else {
        result.fail("ollama chat path");
    }

    if (classify_endpoint_type("/v1/chat/completions", "POST") == "openai") {
        result.ok("openai chat path");
    } else {
        result.fail("openai chat path");
    }

    if (classify_endpoint_type("/api/v1/load", "POST") == "lemonade") {
        result.ok("lemonade load path");
    } else {
        result.fail("lemonade load path");
    }
}

static void test_redaction(TestResult& result) {
    const auto input = nlohmann::json{
        {"model", "demo"},
        {"api_key", "secret-value"},
        {"messages", nlohmann::json::array({nlohmann::json{{"role", "user"}, {"content", "hello"}}})},
    };
    const auto redacted = redact_json(input);
    if (redacted["api_key"] == "[REDACTED]") {
        result.ok("api_key redacted");
    } else {
        result.fail("api_key redacted");
    }
    if (redacted["model"] == "demo") {
        result.ok("model preserved");
    } else {
        result.fail("model preserved");
    }
}

static void test_char_counts_without_prompt_logging(TestResult& result) {
    const std::string body = R"({
        "model": "llama3.2",
        "keep_alive": 0,
        "stream": true,
        "prompt": "hello prompt",
        "messages": [{"role":"user","content":"hello messages"}]
    })";
    const ParsedRequestBody parsed = parse_request_body(body, "/api/generate", false);
    if (parsed.model == "llama3.2" && parsed.keep_alive == "0" &&
        parsed.stream.has_value() && parsed.stream.value() &&
        parsed.prompt_chars == 12 && parsed.messages_chars == 14) {
        result.ok("field extraction and char counts");
    } else {
        result.fail("field extraction and char counts");
    }
    if (parsed.has_redacted_body &&
        parsed.redacted_body["prompt"]["char_count"] == 12 &&
        parsed.redacted_body["messages"]["char_count"] == 14) {
        result.ok("prompt content replaced with char counts");
    } else {
        result.fail("prompt content replaced with char counts");
    }
}

int main() {
    TestResult result;
    test_endpoint_classification(result);
    test_redaction(result);
    test_char_counts_without_prompt_logging(result);

    printf("\nResults: %d passed, %d failed\n", result.passed, result.failed);
    return result.failed == 0 ? 0 : 1;
}
