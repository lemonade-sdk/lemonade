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

    json chat = {{"messages", json::array({
                     json{{"role", "user"}, {"content", "hello"}}})}};
    const int64_t bytes = static_cast<int64_t>(chat["messages"].dump().size());
    check("chat estimate is ceil(bytes/4)",
          cap::estimate_prompt_tokens(chat) == (bytes + 3) / 4);

    json with_tools = chat;
    with_tools["tools"] = json::array({json{{"type", "function"}}});
    check("tools grow the estimate",
          cap::estimate_prompt_tokens(with_tools) > cap::estimate_prompt_tokens(chat));

    json completion = {{"prompt", std::string(4000, 'x')}};
    check("prompt field is counted",
          cap::estimate_prompt_tokens(completion) > 1000);

    json responses_api = {{"input", std::string(4000, 'x')}};
    check("responses input field is counted",
          cap::estimate_prompt_tokens(responses_api) > 1000);

    // Multi-byte UTF-8 counts bytes, not characters: each 'é' is 2 bytes.
    json ascii = {{"prompt", std::string(100, 'a')}};
    json accented = {{"prompt", []{
        std::string s;
        for (int i = 0; i < 100; ++i) s += "\xC3\xA9";
        return s;
    }()}};
    check("estimate counts UTF-8 bytes",
          cap::estimate_prompt_tokens(accented) > cap::estimate_prompt_tokens(ascii));
}

static void test_generation_headroom() {
    check("default headroom when unset",
          cap::generation_headroom(json::object()) == cap::DEFAULT_GENERATION_HEADROOM);
    check("max_tokens wins when set",
          cap::generation_headroom(json{{"max_tokens", 256}}) == 256);
    check("max_completion_tokens honored",
          cap::generation_headroom(json{{"max_completion_tokens", 512}}) == 512);
    check("non-positive max_tokens falls back to default",
          cap::generation_headroom(json{{"max_tokens", 0}}) ==
              cap::DEFAULT_GENERATION_HEADROOM);
}

static void test_fits() {
    check("unknown window (0) always fits", cap::fits(1000000, 1024, 0));
    check("negative window treated as unknown", cap::fits(1000000, 1024, -1));
    // 1.25 * 1000 + 1024 = 2274
    check("exact boundary fits", cap::fits(1000, 1024, 2274));
    check("one below boundary does not fit", !cap::fits(1000, 1024, 2273));
    check("small request fits small window", cap::fits(100, 100, 4096));
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
}

int main() {
    test_estimate_prompt_tokens();
    test_generation_headroom();
    test_fits();
    test_effective_context_window();
    test_is_context_overflow_error();

    if (g_failures > 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All checks passed\n");
    return 0;
}
