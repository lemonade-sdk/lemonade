// Unit tests for the proprietary agent-endpoint translation layer
// (build_envelope / is_model_not_found / feed_event / OpenAI renderers).
// Pure functions only — no network, no registry — so the contract the live
// endpoint probes established stays pinned without a vendor dependency.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <string>

#include <lemon/backends/cloud/alpha_translate.h>

using lemon::backends::cloud::alpha::build_envelope;
using lemon::backends::cloud::alpha::feed_event;
using lemon::backends::cloud::alpha::is_model_not_found;
using lemon::backends::cloud::alpha::new_completion_id;
using lemon::backends::cloud::alpha::sse_content_chunk;
using lemon::backends::cloud::alpha::sse_final_chunk;
using lemon::backends::cloud::alpha::sse_reasoning_chunk;
using lemon::backends::cloud::alpha::sse_role_chunk;
using lemon::backends::cloud::alpha::StreamState;
using lemon::backends::cloud::alpha::to_chat_completion;
using lemon::backends::cloud::alpha::to_text_completion;
using json = nlohmann::json;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

int main() {
    TestResult r;
    printf("=== Alpha translate Unit Tests ===\n\n");

    {
        const json request = {{"model", "commandcode.stepfun/Step-3.5-Flash"},
                              {"messages",
                               {{{"role", "system"}, {"content", "Be brief."}},
                                {{"role", "user"}, {"content", "ping"}}}},
                              {"temperature", 0.7},
                              {"max_tokens", 64}};
        const json envelope = build_envelope("stepfun/Step-3.5-Flash", request);
        const auto& params = envelope["params"];
        r.check(params["model"] == "stepfun/Step-3.5-Flash",
                "build_envelope() passes the upstream model id through");
        r.check(params["stream"] == true,
                "build_envelope() always requests a stream");
        r.check(params["messages"].size() == 1 &&
                    params["messages"][0]["role"] == "user",
                "build_envelope() drops system messages from params.messages");
        r.check(params.value("system", "") == "Be brief.",
                "build_envelope() folds system content into params.system");
        r.check(params.value("temperature", 0.0) == 0.7,
                "build_envelope() passes temperature through");
        r.check(params.value("max_tokens", 0) == 64,
                "build_envelope() passes max_tokens through");
        r.check(envelope["memory"] == "" && envelope.contains("config"),
                "build_envelope() carries the required config/memory envelope");
    }

    {
        const json request = {{"model", "m"}, {"prompt", "complete this"}};
        const json envelope = build_envelope("upstream", request);
        r.check(envelope["params"]["messages"].size() == 1 &&
                    envelope["params"]["messages"][0]["role"] == "user" &&
                    envelope["params"]["messages"][0]["content"] == "complete this",
                "build_envelope() wraps a legacy prompt as a user message");
    }

    {
        const json not_found = {{"error",
                                 {{"message", "Model 'x/y' not found"},
                                  {"type", "model_not_found"}}}};
        r.check(is_model_not_found(404, not_found),
                "is_model_not_found() treats any 404 as a fallback trigger");
        const json unsupported = {
            {"error",
             {{"message", "Model \"Step-3.5-Flash\" is not supported on this "
                         "endpoint."},
              {"type", "invalid_request_error"},
              {"code", "unsupported_model"}}}};
        r.check(is_model_not_found(400, unsupported),
                "is_model_not_found() treats 400/unsupported_model as a trigger");
        // The shape lemond actually sees: Lemonade's own envelope wrapping
        // the provider's double-wrapped 404 body under error.details.
        const json wrapped = json::parse(R"({
            "error": {
                "message": "cloud (commandcode) request failed",
                "type": "backend_error",
                "details": {
                    "status_code": 404,
                    "response": {
                        "error": {
                            "message": "{\"error\":{\"message\":\"Model 'stepfun/step-3.5-flash' not found\"}}",
                            "type": "not_found_error"
                        }
                    }
                }
            }
        })");
        r.check(is_model_not_found(404, wrapped),
                "is_model_not_found() sees through Lemonade's error envelope");
        r.check(is_model_not_found(0, wrapped),
                "is_model_not_found() triggers on a wrapped 404 without status");
        const json auth = {{"error",
                            {{"message", "Invalid API key"},
                             {"type", "auth_error"}}}};
        r.check(!is_model_not_found(401, auth),
                "is_model_not_found() ignores auth errors");
        const json quota = {{"error",
                             {{"message", "rate limit exceeded"},
                              {"type", "rate_limit"}}}};
        r.check(!is_model_not_found(429, quota),
                "is_model_not_found() ignores quota errors");
    }

    {
        StreamState state;
        feed_event(json{{"type", "reasoning-delta"}, {"text", "thinking "}}, state);
        feed_event(json{{"type", "text-delta"}, {"text", "ok"}}, state);
        feed_event(json{{"type", "finish-step"},
                        {"finishReason", "stop"},
                        {"usage",
                         {{"inputTokens", 100},
                          {"outputTokens", 5},
                          {"totalTokens", 105}}}},
                   state);
        r.check(state.reasoning == "thinking " && state.text == "ok",
                "feed_event() accumulates reasoning and text deltas");
        r.check(state.input_tokens == 100 && state.output_tokens == 5,
                "feed_event() reads camelCase usage from finish-step");
        r.check(!state.error, "feed_event() leaves the error flag clear");
        feed_event(json{{"type", "some-future-event"}, {"text", "x"}}, state);
        r.check(state.text == "ok",
                "feed_event() ignores unknown event types");
    }

    {
        StreamState state;
        state.text = "ok";
        state.reasoning = "why ok";
        state.input_tokens = 10;
        state.output_tokens = 2;
        const json out = to_chat_completion(state, "m", "chatcmpl-alpha-1", 123);
        r.check(out["object"] == "chat.completion" && out["id"] == "chatcmpl-alpha-1",
                "to_chat_completion() renders the OpenAI envelope");
        r.check(out["choices"][0]["message"]["content"] == "ok",
                "to_chat_completion() carries the accumulated text");
        r.check(out["choices"][0]["message"]["reasoning_content"] == "why ok",
                "to_chat_completion() carries reasoning_content");
        r.check(out["usage"]["total_tokens"] == 12,
                "to_chat_completion() renders usage totals");
        r.check(out["choices"][0]["finish_reason"] == "stop",
                "to_chat_completion() defaults finish_reason to stop");
    }

    {
        StreamState state;
        state.text = "done";
        const json out = to_text_completion(state, "m", "chatcmpl-alpha-2", 123);
        r.check(out["object"] == "text_completion" &&
                    out["choices"][0]["text"] == "done",
                "to_text_completion() renders the legacy envelope");
    }

    {
        const std::string id = new_completion_id();
        r.check(id.rfind("chatcmpl-alpha-", 0) == 0,
                "new_completion_id() uses the chatcmpl-alpha- prefix");
        r.check(new_completion_id() != id, "new_completion_id() is unique");
    }

    {
        StreamState state;
        state.text = "hi";
        const std::string role = sse_role_chunk("id", 1, "m", false);
        const std::string content = sse_content_chunk("id", 1, "m", "hi", false);
        const std::string reasoning = sse_reasoning_chunk("id", 1, "m", "why");
        const std::string final = sse_final_chunk(state, "id", 1, "m", false, true);
        r.check(role.rfind("data: ", 0) == 0 && role.find("assistant") != std::string::npos,
                "sse_role_chunk() emits an assistant role chunk");
        r.check(content.find("\"content\":\"hi\"") != std::string::npos,
                "sse_content_chunk() emits a content delta chunk");
        r.check(reasoning.find("reasoning_content") != std::string::npos,
                "sse_reasoning_chunk() emits a reasoning_content chunk");
        r.check(final.find("\"finish_reason\":\"stop\"") != std::string::npos &&
                    final.find("\"usage\"") != std::string::npos,
                "sse_final_chunk() emits finish_reason with usage when asked");
        const std::string no_usage = sse_final_chunk(state, "id", 1, "m", false, false);
        r.check(no_usage.find("\"usage\"") == std::string::npos,
                "sse_final_chunk() omits usage when not requested");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
