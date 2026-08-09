#include "lemon/anthropic_error.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using lemon::anthropic::backend_error_http_status;
using lemon::anthropic::build_anthropic_error;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        printf("[%s] %s\n", condition ? "PASS" : "FAIL", name.c_str());
        if (condition) {
            ++passed;
        } else {
            ++failed;
        }
    }
};

static void test_status_extraction(TestResult& r) {
    r.check(backend_error_http_status(json{{"status_code", 429}}) == 429,
            "status_code wins");
    r.check(backend_error_http_status(json{{"code", 413}}) == 413,
            "llama.cpp numeric code");
    r.check(backend_error_http_status(json{{"details", {{"status_code", 504}}}}) == 504,
            "nested details.status_code");
    r.check(backend_error_http_status(json{{"status_code", 200}}) == 500,
            "non-error status_code ignored");
    r.check(backend_error_http_status(json{{"status_code", "429"}}) == 500,
            "non-integer status_code ignored");
    r.check(backend_error_http_status(json{{"type", "invalid_request_error"}}) == 400,
            "type invalid_request_error");
    r.check(backend_error_http_status(json{{"type", "unsupported_operation"}}) == 400,
            "type unsupported_operation");
    r.check(backend_error_http_status(json{{"type", "model_not_loaded"}}) == 404,
            "type model_not_loaded");
    r.check(backend_error_http_status(json{{"type", "backend_error"}}) == 500,
            "unmapped type falls back to 500");
    r.check(backend_error_http_status(json("not an object")) == 500,
            "non-object error falls back to 500");
}

static std::string error_type_for(int status) {
    return build_anthropic_error(json{{"message", "boom"}}, status)["error"]["type"]
        .get<std::string>();
}

static void test_error_type_mapping(TestResult& r) {
    struct Case {
        int status;
        const char* type;
    };
    const Case cases[] = {
        {400, "invalid_request_error"},
        {401, "authentication_error"},
        {402, "billing_error"},
        {403, "permission_error"},
        {404, "not_found_error"},
        {409, "conflict_error"},
        {413, "request_too_large"},
        {422, "invalid_request_error"},
        {429, "rate_limit_error"},
        {499, "invalid_request_error"},
        {500, "api_error"},
        {502, "api_error"},
        {504, "timeout_error"},
        {529, "overloaded_error"},
    };

    for (const auto& c : cases) {
        r.check(error_type_for(c.status) == c.type,
                std::to_string(c.status) + " -> " + c.type);
    }
}

static void test_envelope(TestResult& r) {
    const json wrapped = build_anthropic_error(json{{"message", "context overflow"}}, 400);
    r.check(wrapped.value("type", "") == "error", "envelope type is error");
    r.check(wrapped["error"].value("message", "") == "context overflow",
            "backend message survives");

    const json without_message = build_anthropic_error(json::object(), 500);
    r.check(without_message["error"].value("message", "") == "backend error",
            "missing message falls back");

    const json non_object = build_anthropic_error(json("raw failure"), 500);
    r.check(non_object["error"].value("message", "") == "\"raw failure\"",
            "non-object error is dumped into the message");
}

int main() {
    TestResult r;
    printf("=== Anthropic Error Mapping Unit Tests ===\n\n");

    test_status_extraction(r);
    test_error_type_mapping(r);
    test_envelope(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
