// Unit tests for the OpenAI chat message "content" extraction helper.
//
// A reasoning model whose output budget is exhausted inside its thinking
// answers with content: null instead of content: "". The chat completion
// response path used to read that field with an unguarded
// .get<std::string>(), which throws nlohmann type_error.302 and turned the
// successful completion into a 500. The helper must return the content
// only when it is actually a string.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include "lemon/message_content.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using lemon::openai::message_content;

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

static void test_string_content(TestResult& r) {
    const auto hello = message_content(json{{"content", "hello"}});
    r.check(hello.has_value() && *hello == "hello", "string content is returned");

    const auto empty = message_content(json{{"content", ""}});
    r.check(empty.has_value() && *empty == std::string{}, "empty string content is returned");

    const auto with_tools =
        message_content(json{{"content", "answer"}, {"tool_calls", json::array()}});
    r.check(with_tools.has_value() && *with_tools == "answer", "string content alongside other fields");
}

static void test_non_string_content(TestResult& r) {
    json null_content;
    null_content["content"] = nullptr;
    r.check(!message_content(null_content).has_value(), "null content -> nullopt, no throw");

    r.check(!message_content(json::object()).has_value(), "missing content -> nullopt");
    r.check(!message_content(json{{"content", 42}}).has_value(), "integer content -> nullopt");
    r.check(!message_content(json{{"content", {1, 2, 3}}}).has_value(), "array content -> nullopt");
    r.check(!message_content(json{{"content", json::object()}}).has_value(),
            "object content -> nullopt");
}

static void test_non_object_message(TestResult& r) {
    r.check(!message_content(nullptr).has_value(), "null message -> nullopt, no throw");
    r.check(!message_content(json("not an object")).has_value(), "string message -> nullopt, no throw");
    r.check(!message_content(json::array({"content", "x"})).has_value(),
            "array message -> nullopt, no throw");
}

int main() {
    TestResult r;
    printf("=== OpenAI Message Content Unit Tests ===\n\n");

    test_string_content(r);
    test_non_string_content(r);
    test_non_object_message(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
