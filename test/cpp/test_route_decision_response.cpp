#include "lemon/route_decision_response.h"

#include <cstdio>
#include <string>
#include <vector>

using lemon::Decision;
using lemon::RouteDecisionSseSink;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static json decision_json() {
    Decision decision;
    decision.route_to = "Tiny-Test-Model-GGUF";
    decision.matched_rule = "code-to-test-model";
    return lemon::route_decision_to_json(decision);
}

struct SinkCapture {
    httplib::DataSink inner;
    std::string body;
    bool done = false;

    SinkCapture() {
        inner.write = [this](const char* data, size_t len) {
            body.append(data, len);
            return true;
        };
        inner.done = [this]() { done = true; };
        inner.is_writable = []() { return true; };
    }
};

static bool output_contains_route(const std::string& body) {
    return body.find("\"x_lemonade_route\"") != std::string::npos &&
           body.find("\"route_to\":\"Tiny-Test-Model-GGUF\"") != std::string::npos;
}

static void test_lf_event_is_injected() {
    SinkCapture capture;
    RouteDecisionSseSink sink(capture.inner, decision_json());

    const std::string event = "data: {\"id\":\"one\"}\n\n";
    check("LF write succeeds", sink.write(event.data(), event.size()));
    check("LF event flushes before done", output_contains_route(capture.body));
    check("LF event does not call done early", !capture.done);

    sink.done();
    check("LF done forwarded", capture.done);
}

static void test_crlf_split_event_is_injected_before_done() {
    SinkCapture capture;
    RouteDecisionSseSink sink(capture.inner, decision_json());

    const std::vector<std::string> chunks = {
        "da",
        "ta: {\"id\":\"two\"}\r",
        "\n\r",
        "\n",
    };
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        check("CRLF split write succeeds", sink.write(chunks[i].data(), chunks[i].size()));
        if (i + 1 < chunks.size()) {
            check("CRLF split waits for full record separator", capture.body.empty());
        }
    }

    check("CRLF split event flushes before done", output_contains_route(capture.body));
    check("CRLF output preserves CRLF framing", capture.body.find("\r\n\r\n") != std::string::npos);
    check("CRLF event does not call done early", !capture.done);
}

static void test_cr_event_is_injected() {
    SinkCapture capture;
    RouteDecisionSseSink sink(capture.inner, decision_json());

    const std::string event = "data: {\"id\":\"three\"}\r\r";
    check("CR write succeeds", sink.write(event.data(), event.size()));
    check("CR event flushes before done", output_contains_route(capture.body));
    check("CR output preserves CR framing", capture.body.find("\r\r") != std::string::npos);
}

static void test_only_first_json_event_gets_route() {
    SinkCapture capture;
    RouteDecisionSseSink sink(capture.inner, decision_json());

    const std::string events =
        "data: {\"id\":\"first\"}\n\n"
        "data: {\"id\":\"second\"}\n\n";
    check("multi-event write succeeds", sink.write(events.data(), events.size()));

    const std::size_t first = capture.body.find("\"x_lemonade_route\"");
    check("first event receives route", first != std::string::npos);
    check("route attached only once",
          first == capture.body.rfind("\"x_lemonade_route\""));
}

static void test_route_header_default_value() {
    Decision matched;
    matched.matched_rule = "safe-rule_1.2";
    check("matched rule header uses rule id",
          lemon::route_decision_header_value(matched) == "safe-rule_1.2");

    Decision defaulted;
    defaulted.default_used = true;
    check("default route header is explicit",
          lemon::route_decision_header_value(defaulted) == "default");
}

// #2405: trace entries may carry the tested label and the classifier's
// rationale (the llm router's pick reason). Both are optional, serialized only
// when non-empty, and both are declared in decision.schema.json's trace_entry
// (which uses additionalProperties:false — a key emitted here but missing from
// the schema would make every such response schema-invalid).
static void test_trace_serializes_label_and_rationale() {
    Decision decision;
    decision.route_to = "builtin.Qwen3.5-35B-A3B-GGUF";
    decision.matched_rule = "__route_1";
    decision.trace.push_back(lemon::TraceEntry{
        "classifier:__router", 1.0, true, "Qwen3.5-35B-A3B-GGUF",
        "Qwen3.5-35B-A3B-GGUF"});
    decision.trace.push_back(lemon::TraceEntry{"keywords_any", std::nullopt, false});

    json out = lemon::route_decision_to_json(decision);
    const json& first = out["trace"][0];
    check("trace entry serializes label",
          first.value("label", "") == "Qwen3.5-35B-A3B-GGUF");
    check("trace entry serializes rationale",
          first.value("rationale", "") == "Qwen3.5-35B-A3B-GGUF");
    check("empty label/rationale keys are omitted",
          !out["trace"][1].contains("label") && !out["trace"][1].contains("rationale"));
}

int main() {
    test_lf_event_is_injected();
    test_crlf_split_event_is_injected_before_done();
    test_cr_event_is_injected();
    test_only_first_json_event_gets_route();
    test_route_header_default_value();
    test_trace_serializes_label_and_rationale();

    if (g_failures == 0) {
        std::printf("All route decision response tests passed.\n");
    } else {
        std::printf("%d route decision response test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
