// Unit tests for request log parser helpers.

#include "lemon/request_log_parser.h"

#include <cassert>
#include <cstdio>
#include <string>

using lemon::ParsedRequestBody;
using lemon::classify_endpoint_type;
using lemon::extract_response_error;
using lemon::parse_request_body;
using lemon::parse_response_body;
using lemon::redact_json;
using lemon::safe_json_dump;
using lemon::sanitize_utf8_for_db;

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

static void test_binary_response_error(TestResult& result) {
    const std::string gzip_like = std::string{'\x1f', '\x8b', '\x08', '\x00'};
    const std::string extracted = extract_response_error(gzip_like, 404);
    if (extracted.find("non-UTF-8") != std::string::npos) {
        result.ok("binary response error sanitized");
    } else {
        result.fail("binary response error sanitized");
    }

    const std::string sanitized = sanitize_utf8_for_db(std::string{'\x8b', 'x'});
    if (sanitized.find('\x8b') == std::string::npos && !sanitized.empty()) {
        result.ok("invalid utf8 bytes replaced");
    } else {
        result.fail("invalid utf8 bytes replaced");
    }
}

static void test_response_tokens_and_content(TestResult& result) {
    const std::string openai_response = R"({
        "choices": [{"message": {"role": "assistant", "content": "Hello there"}}],
        "usage": {"prompt_tokens": 12, "completion_tokens": 3}
    })";
    const auto parsed = lemon::parse_response_body(openai_response, "/v1/chat/completions", 200, true);
    if (parsed.prompt_tokens.has_value() && parsed.prompt_tokens.value() == 12 &&
        parsed.completion_tokens.has_value() && parsed.completion_tokens.value() == 3 &&
        parsed.has_redacted_response) {
        result.ok("openai response tokens extracted");
    } else {
        result.fail("openai response tokens extracted");
    }

    const std::string ollama_response = R"({
        "message": {"role": "assistant", "content": "Hi"},
        "prompt_eval_count": 20,
        "eval_count": 2
    })";
    const auto ollama_parsed =
        lemon::parse_response_body(ollama_response, "/api/chat", 200, false);
    if (ollama_parsed.prompt_tokens.has_value() && ollama_parsed.prompt_tokens.value() == 20 &&
        ollama_parsed.completion_tokens.has_value() && ollama_parsed.completion_tokens.value() == 2) {
        result.ok("ollama response tokens extracted");
    } else {
        result.fail("ollama response tokens extracted");
    }
}

static void test_invalid_utf8_json_dump(TestResult& result) {
    nlohmann::json summary = nlohmann::json::object();
    std::string bad = "hi";
    bad.push_back(static_cast<char>(0xF6));
    summary["content"] = sanitize_utf8_for_db(bad);
    const std::string dumped = safe_json_dump(summary);
    if (!dumped.empty() && dumped.find("content") != std::string::npos) {
        result.ok("sanitized invalid utf8 dumps safely");
    } else {
        result.fail("sanitized invalid utf8 dumps safely");
    }
}

static void test_invalid_utf8_response_content(TestResult& result) {
    std::string content = "answer";
    content.push_back(static_cast<char>(0xF6));
    const nlohmann::json response_json = {
        {"message", nlohmann::json{{"role", "assistant"}, {"content", content}}},
        {"prompt_eval_count", 42},
        {"eval_count", 7},
    };
    const std::string response =
        response_json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const auto parsed = parse_response_body(response, "/api/chat", 200, true);
    if (parsed.prompt_tokens.has_value() && parsed.prompt_tokens.value() == 42 &&
        parsed.completion_tokens.has_value() && parsed.completion_tokens.value() == 7 &&
        parsed.has_redacted_response &&
        parsed.redacted_response.contains("content") &&
        !parsed.redacted_response.contains("note")) {
        result.ok("invalid utf8 response content logged without serialization fallback");
    } else {
        result.fail("invalid utf8 response content logged without serialization fallback");
    }
}

static void test_float_token_counts(TestResult& result) {
    const std::string response = R"({
        "message": {"role": "assistant", "content": "ok"},
        "prompt_eval_count": 100.0,
        "eval_count": 5.0
    })";
    const auto parsed = parse_response_body(response, "/api/chat", 200, false);
    if (parsed.prompt_tokens.has_value() && parsed.prompt_tokens.value() == 100 &&
        parsed.completion_tokens.has_value() && parsed.completion_tokens.value() == 5) {
        result.ok("float token counts coerced to integers");
    } else {
        result.fail("float token counts coerced to integers");
    }
}

int main() {
    TestResult result;
    test_endpoint_classification(result);
    test_redaction(result);
    test_char_counts_without_prompt_logging(result);
    test_binary_response_error(result);
    test_response_tokens_and_content(result);
    test_invalid_utf8_json_dump(result);
    test_invalid_utf8_response_content(result);
    test_float_token_counts(result);

    printf("\nResults: %d passed, %d failed\n", result.passed, result.failed);
    return result.failed == 0 ? 0 : 1;
}
