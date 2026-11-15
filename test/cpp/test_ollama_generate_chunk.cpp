#include "lemon/server/gateway_conversion.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using lemon::ollama_generate_chunk;

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

static std::string response_of(const json& openai_chunk) {
    const json line = ollama_generate_chunk(openai_chunk, "test-model");
    return line.contains("response") && line["response"].is_string()
        ? line["response"].get<std::string>()
        : "<not a string>";
}

static void test_response_is_always_a_string(TestResult& r) {
    r.check(response_of(json::parse(R"({"choices":[{"text":"Hi"}]})")) == "Hi",
            "completion chunk text");
    r.check(response_of(json::parse(R"({"choices":[{"delta":{"content":"Hi"}}]})")) == "Hi",
            "chat chunk content");
    r.check(response_of(json::parse(R"({"choices":[{"delta":{"role":"assistant","content":null}}]})")) == "",
            "chat chunk with null content");
    r.check(response_of(json::parse(R"({"choices":[],"usage":{"prompt_tokens":5}})")) == "",
            "usage chunk with empty choices");
    r.check(response_of(json::parse(R"({"usage":{"prompt_tokens":5}})")) == "",
            "chunk without choices");
}

static void test_finish_reason_ends_the_line(TestResult& r) {
    const json last = ollama_generate_chunk(
        json::parse(R"({"choices":[{"delta":{},"finish_reason":"length"}]})"), "test-model");
    r.check(last.value("done", false), "finish_reason sets done");
    r.check(last.value("done_reason", "") == "length", "finish_reason becomes done_reason");

    const json middle = ollama_generate_chunk(
        json::parse(R"({"choices":[{"delta":{"content":"Hi"},"finish_reason":null}]})"), "test-model");
    r.check(!middle.value("done", true), "null finish_reason keeps done false");
    r.check(middle.value("model", "") == "test-model", "line names the model");
}

int main() {
    TestResult r;
    printf("=== Ollama Generate Chunk Unit Tests ===\n\n");

    test_response_is_always_a_string(r);
    test_finish_reason_ends_the_line(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
