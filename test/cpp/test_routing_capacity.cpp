// Unit tests for the router-collection capacity helpers (#2959).
//
// Covers the prompt-token estimator (messages / system / tools / prompt /
// input, UTF-8 byte counting), generation headroom resolution, the
// effective-context-window decision tree (loaded vs cloud vs pinned vs
// unloaded-llamacpp vs unknown), and the fit predicate's safety margin.
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       test/cpp/test_routing_capacity.cpp src/cpp/server/routing_capacity.cpp \
//       -o test_routing_capacity

#include "lemon/auto_tune.h"
#include "lemon/routing_capacity.h"

#include <cstdio>
#include <string>

using lemon::ModelInfo;
using lemon::json;
namespace cap = lemon::routing_capacity;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void test_estimate_prompt_tokens() {
    check("empty request estimates zero",
          cap::estimate_prompt_tokens(json::object()) == 0);
    check("non-object request estimates zero",
          cap::estimate_prompt_tokens(json("hi")) == 0);

    // Content text drives the estimate; the JSON envelope's keys and quotes are
    // never sent to the model and must not be counted.
    const std::string content(400, 'a');
    json chat = {{"messages", json::array({
                     json{{"role", "user"}, {"content", content}}})}};
    const int64_t expected = static_cast<int64_t>(content.size()) / 4 +
                             cap::TOKENS_PER_MESSAGE_OVERHEAD;
    check("chat estimate is content bytes/4 plus per-message overhead",
          cap::estimate_prompt_tokens(chat) == expected);
    check("chat estimate excludes the JSON envelope",
          cap::estimate_prompt_tokens(chat) <
              static_cast<int64_t>(chat["messages"].dump().size()) / 4);

    // A long role name or extra keys must not inflate the estimate.
    json verbose = {{"messages", json::array({
                        json{{"role", "user"},
                             {"content", content},
                             {"some_client_annotation", std::string(500, 'z')}}})}};
    check("unknown message keys are not counted",
          cap::estimate_prompt_tokens(verbose) == expected);

    // Content parts: text counts, an image blob does not (it never enters the
    // prompt as text, and counting its base64 would wildly over-skip).
    json parts = {{"messages", json::array({
                      json{{"role", "user"},
                           {"content", json::array({
                               json{{"type", "text"}, {"text", content}},
                               json{{"type", "image_url"},
                                    {"image_url", {{"url", std::string(5000, 'Z')}}}}})}}})}};
    check("content parts count text and skip image blobs",
          cap::estimate_prompt_tokens(parts) == expected);

    json with_tools = chat;
    with_tools["tools"] = json::array({json{{"type", "function"}}});
    check("tool schemas grow the estimate (they are injected as JSON)",
          cap::estimate_prompt_tokens(with_tools) > cap::estimate_prompt_tokens(chat));

    json with_tool_calls = {{"messages", json::array({
                                json{{"role", "assistant"},
                                     {"tool_calls", json::array({
                                         json{{"id", "call_1"}}})}}})}};
    check("assistant tool_calls are counted",
          cap::estimate_prompt_tokens(with_tool_calls) >
              cap::TOKENS_PER_MESSAGE_OVERHEAD);

    json completion = {{"prompt", std::string(4000, 'x')}};
    check("prompt field is counted", cap::estimate_prompt_tokens(completion) == 1000);

    json responses_api = {{"input", std::string(4000, 'x')}};
    check("responses input field is counted",
          cap::estimate_prompt_tokens(responses_api) == 1000);

    json system_only = {{"system", std::string(400, 's')}};
    check("system field is counted", cap::estimate_prompt_tokens(system_only) == 100);

    // Multi-byte UTF-8 counts bytes, not characters: each 'é' is 2 bytes.
    json ascii = {{"prompt", std::string(100, 'a')}};
    json accented = {{"prompt", []{
        std::string s;
        for (int i = 0; i < 100; ++i) s += "\xC3\xA9";
        return s;
    }()}};
    check("estimate counts UTF-8 bytes",
          cap::estimate_prompt_tokens(accented) > cap::estimate_prompt_tokens(ascii));

    // Each message pays the template overhead, so many short turns cost more
    // than one long turn of the same total text.
    json many = {{"messages", json::array()}};
    for (int i = 0; i < 10; ++i) {
        many["messages"].push_back(json{{"role", "user"}, {"content", std::string(40, 'a')}});
    }
    check("per-message overhead accumulates",
          cap::estimate_prompt_tokens(many) == 400 / 4 + 10 * cap::TOKENS_PER_MESSAGE_OVERHEAD);
}

static void test_generation_headroom() {
    const int64_t policy_default = lemon::CapacitySettings{}.generation_headroom;
    check("policy fallback used when request sets none",
          cap::generation_headroom(json::object(), policy_default) == policy_default);
    check("max_tokens wins when set",
          cap::generation_headroom(json{{"max_tokens", 256}}, policy_default) == 256);
    check("max_completion_tokens honored",
          cap::generation_headroom(json{{"max_completion_tokens", 512}}, policy_default) == 512);
    check("non-positive max_tokens falls back to the policy value",
          cap::generation_headroom(json{{"max_tokens", 0}}, policy_default) == policy_default);
    check("a policy may raise the fallback",
          cap::generation_headroom(json::object(), 4096) == 4096);
}

static void test_fits() {
    const double margin = lemon::CapacitySettings{}.safety_margin;
    check("unknown window (0) always fits", cap::fits(1000000, 1024, 0, margin));
    check("negative window treated as unknown", cap::fits(1000000, 1024, -1, margin));
    // 1.25 * 1000 + 1024 = 2274
    check("exact boundary fits", cap::fits(1000, 1024, 2274, margin));
    check("one below boundary does not fit", !cap::fits(1000, 1024, 2273, margin));
    check("small request fits small window", cap::fits(100, 100, 4096, margin));

    // A policy-tuned margin moves the boundary: at 1.0 the same request fits a
    // window the default margin would have skipped.
    check("margin 1.0 admits what 1.25 rejects",
          cap::fits(1000, 1024, 2100, 1.0) && !cap::fits(1000, 1024, 2100, margin));
    check("a larger margin skips more aggressively",
          !cap::fits(1000, 1024, 2274, 2.0));
}

static void test_effective_context_window() {
    ModelInfo local;
    local.recipe = "llamacpp";
    local.max_context_window = 32768;

    check("loaded ctx_size wins",
          cap::effective_context_window(local, 8192, std::nullopt, 0.0) == 8192);
    check("pinned ctx_size wins when not loaded",
          cap::effective_context_window(local, std::nullopt, 16384, 0.0) == 16384);
    // Unknown memory: compute_auto_context_size resolves the fallback window,
    // exactly what the load path would apply.
    check("unloaded llamacpp with unknown memory uses auto-tune fallback",
          cap::effective_context_window(local, std::nullopt, std::nullopt, 0.0) ==
              lemon::AUTO_CTX_FALLBACK);
    // Plenty of memory: the estimate is clamped to the registry max window.
    check("unloaded llamacpp with ample memory clamps to model max",
          cap::effective_context_window(local, std::nullopt, std::nullopt, 4096.0) ==
              local.max_context_window);

    ModelInfo cloud;
    cloud.recipe = "cloud";
    cloud.max_context_window = 131072;
    check("cloud uses provider-reported window",
          cap::effective_context_window(cloud, std::nullopt, std::nullopt, 0.0) == 131072);

    ModelInfo cloud_unknown;
    cloud_unknown.recipe = "cloud";
    check("cloud with no reported window is unconstrained",
          cap::effective_context_window(cloud_unknown, std::nullopt, std::nullopt, 0.0) == 0);

    ModelInfo other;
    other.recipe = "flm";
    other.max_context_window = 4096;
    check("non-llamacpp local uses registry max window",
          cap::effective_context_window(other, std::nullopt, std::nullopt, 0.0) == 4096);

    ModelInfo other_unknown;
    other_unknown.recipe = "flm";
    check("non-llamacpp local with no window is unconstrained",
          cap::effective_context_window(other_unknown, std::nullopt, std::nullopt, 0.0) == 0);
}

static void test_is_context_overflow_error() {
    check("success response is not an overflow error",
          !cap::is_context_overflow_error(json{{"choices", json::array()}}));
    check("unrelated backend error is not an overflow error",
          !cap::is_context_overflow_error(
              json{{"error", {{"message", "connection refused"},
                              {"code", "backend_error"}}}}));
    check("normalized code is detected",
          cap::is_context_overflow_error(
              json{{"error", {{"message", "too long"},
                              {"code", "context_length_exceeded"}}}}));
    // A cloud provider's rejection keeps its raw body nested under details and
    // carries only a generic backend_error code.
    check("nested provider rejection text is detected",
          cap::is_context_overflow_error(json{
              {"error",
               {{"message", "cloud (x) request failed"},
                {"type", "backend_error"},
                {"details",
                 {{"status_code", 400},
                  {"response",
                   {{"error",
                     {{"message",
                       "request (9000 tokens) exceeds the available context "
                       "size (512 tokens), try increasing it"}}}}}}}}}}));
    check("llama.cpp rejection text is detected regardless of case",
          cap::is_context_overflow_error(
              json{{"error", {{"message", "Context Length Exceeded"}}}}));

    // The text fallback must only read the fields that carry backend
    // rejections. An error that echoes user input — a prompt asking about the
    // error code, a model id, an unrelated details payload — must not evict a
    // working candidate.
    check("user input echoed in a non-message field does not match",
          !cap::is_context_overflow_error(json{
              {"error",
               {{"message", "invalid request"},
                {"type", "invalid_request_error"},
                {"param", "why did I get context_length_exceeded?"}}}}));
    check("the bare code word in free text does not match",
          !cap::is_context_overflow_error(json{
              {"error", {{"message", "unknown field context_length_exceeded"}}}}));
    check("a request echo under details does not match",
          !cap::is_context_overflow_error(json{
              {"error",
               {{"message", "validation failed"},
                {"details",
                 {{"request",
                   {{"messages",
                     "explain the error exceeds the available context size"}}}}}}}}));
    check("a nested provider code is detected",
          cap::is_context_overflow_error(json{
              {"error",
               {{"message", "cloud (x) request failed"},
                {"details",
                 {{"response",
                   {{"error", {{"code", "context_length_exceeded"}}}}}}}}}}));
}

static void test_sse_event_is_context_overflow() {
    check("an SSE error event is detected",
          cap::sse_event_is_context_overflow(
              "data: {\"error\":{\"message\":\"request (9000 tokens) exceeds the "
              "available context size (512 tokens), try increasing it\","
              "\"status_code\":400}}\n\n"));
    check("a normal content chunk is not an overflow",
          !cap::sse_event_is_context_overflow(
              "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"));
    check("the DONE sentinel is not an overflow",
          !cap::sse_event_is_context_overflow("data: [DONE]\n\n"));
    check("a non-SSE line is not an overflow",
          !cap::sse_event_is_context_overflow("garbage\n\n"));
    check("an unrelated SSE error is not an overflow",
          !cap::sse_event_is_context_overflow(
              "data: {\"error\":{\"message\":\"connection reset\"}}\n\n"));
}

int main() {
    test_estimate_prompt_tokens();
    test_generation_headroom();
    test_fits();
    test_effective_context_window();
    test_is_context_overflow_error();
    test_sse_event_is_context_overflow();

    if (g_failures > 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All checks passed\n");
    return 0;
}
