#include "lemon/thinking_controls.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using lemon::normalize_thinking_controls;
using lemon::should_disable_thinking;

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

static json user_request(json extra) {
    json request = {
        {"model", "Qwen3-4B-GGUF"},
        {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
    };
    request.update(extra);
    return request;
}

static bool disables_thinking(const json& request) {
    return request.value("reasoning_effort", "") == "none" &&
           request.value("chat_template_kwargs", json::object()).value("enable_thinking", true) == false &&
           request["messages"][0]["content"].get<std::string>().rfind("/no_think", 0) == 0 &&
           !request.contains("enable_thinking") &&
           !request.contains("thinking");
}

static void test_intent_detection(TestResult& r) {
    r.check(should_disable_thinking(json{{"enable_thinking", false}}), "enable_thinking false");
    r.check(!should_disable_thinking(json{{"enable_thinking", true}}), "enable_thinking true");
    r.check(should_disable_thinking(json{{"thinking", false}}), "thinking false");
    r.check(should_disable_thinking(json{{"thinking", {{"type", "disabled"}}}}), "thinking disabled");
    r.check(!should_disable_thinking(json{{"thinking", {{"type", "enabled"}}}}), "thinking enabled");
    r.check(should_disable_thinking(json{{"enable_thinking", false}, {"thinking", true}}),
            "enable_thinking wins over thinking");
    r.check(!should_disable_thinking(json::object()), "absent means no opinion");
}

// The shape every dialect translation layer (Ollama think=false, Anthropic
// thinking.type=disabled, MCP, OpenAI) relies on to reach llama.cpp.
static void test_disable_translation(TestResult& r) {
    json request = user_request({{"enable_thinking", false}});
    r.check(normalize_thinking_controls(request), "disable reports a modification");
    r.check(disables_thinking(request), "disable sets every native control");

    json anthropic = user_request({{"thinking", {{"type", "disabled"}}}});
    normalize_thinking_controls(anthropic);
    r.check(disables_thinking(anthropic), "anthropic disabled form translates");
}

static void test_no_op_paths(TestResult& r) {
    json untouched = user_request(json::object());
    r.check(!normalize_thinking_controls(untouched), "no thinking fields is a no-op");
    r.check(!untouched.contains("reasoning_effort"), "no-op leaves reasoning_effort unset");

    json enabled = user_request({{"enable_thinking", true}});
    normalize_thinking_controls(enabled);
    r.check(!enabled.contains("reasoning_effort"), "enabling does not pin reasoning_effort");
    r.check(!enabled.contains("enable_thinking"), "client-facing field is stripped");
    r.check(enabled["messages"][0]["content"] == "hi", "enabling leaves the prompt alone");

    // Ollama's think:"low" arrives as reasoning_effort; nothing should clobber it.
    json effort = user_request({{"reasoning_effort", "low"}});
    normalize_thinking_controls(effort);
    r.check(effort.value("reasoning_effort", "") == "low", "explicit effort survives");
}

static void test_idempotent(TestResult& r) {
    json request = user_request({{"enable_thinking", false}});
    normalize_thinking_controls(request);
    const json once = request;
    r.check(!normalize_thinking_controls(request), "second pass changes nothing");
    r.check(request == once, "second pass does not re-prefix /no_think");
}

static void test_preserves_caller_kwargs(TestResult& r) {
    json request = user_request({
        {"enable_thinking", false},
        {"chat_template_kwargs", {{"custom", 1}}},
    });
    normalize_thinking_controls(request);
    const json kwargs = request.value("chat_template_kwargs", json::object());
    r.check(kwargs.value("custom", 0) == 1, "existing kwargs survive");
    r.check(kwargs.value("enable_thinking", true) == false,
            "enable_thinking merged into existing kwargs");
}

int main() {
    TestResult r;
    printf("=== Thinking Controls Unit Tests ===\n\n");

    test_intent_detection(r);
    test_disable_translation(r);
    test_no_op_paths(r);
    test_idempotent(r);
    test_preserves_caller_kwargs(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
