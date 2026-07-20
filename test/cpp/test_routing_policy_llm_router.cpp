// Unit + end-to-end tests for the `llm` router classifier and the
// `routing.router` (L0a) desugaring (#2405).
//
// Covers: the llm classifier reports the model's chosen candidate as label
// (score 1.0) + rationale; a non-candidate reply yields an empty Score so the
// engine fails open to default_model; a backend failure yields Score::ok=false;
// the parser desugars routing.router into one llm classifier + identity rules;
// and the full engine routes an L0a policy end-to-end with the pick + rationale
// in the trace. All backend access is faked via FakeClassifierServices.
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       test/cpp/test_routing_policy_llm_router.cpp \
//       src/cpp/server/routing_policy.cpp src/cpp/server/routing_policy_parser.cpp \
//       -o test_routing_policy_llm_router

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::ClassifierContext;
using lemon::ClassifierPtr;
using lemon::ClassifierServices;
using lemon::Decision;
using lemon::RouteContext;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::RoutingPolicyParseOptions;
using lemon::Score;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static RouteContext make_route(const std::string& input) {
    RouteContext route;
    route.input = input;
    route.params.model = "user.Router-Auto";
    route.params.chars = input.size();
    return route;
}

// The L0a example from the issue / the l0a_llm_router.json fixture, built inline
// so the test has no working-directory dependency.
static json l0a_collection() {
    return json{
        {"version", "1"},
        {"model_name", "user.Router-Auto"},
        {"recipe", "collection.router"},
        {"components", {"Qwen3-1.7B-GGUF", "Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"}},
        {"routing", {
            {"candidates", {"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"}},
            {"default_model", "Qwen3-8B-GGUF"},
            {"router", {
                {"type", "llm"},
                {"model", "Qwen3-1.7B-GGUF"},
                {"prompt", "Pick the best model for the user's request."},
            }},
        }},
    };
}

static RoutePolicy parse_l0a(const json& collection) {
    RoutingPolicyParseOptions options;
    // Identity resolver: component names route to themselves.
    options.resolve_component = [](const std::string& c) {
        return std::optional<std::string>(c);
    };
    return lemon::parse_route_policy_collection(collection, options);
}

// ---------------------------------------------------------------------------
// Classifier-level behavior
// ---------------------------------------------------------------------------

static ClassifierPtr make_llm(const std::vector<std::string>& candidates) {
    json cfg = {
        {"id", "__router"},
        {"type", "llm"},
        {"model", "router-model"},
        {"prompt", "pick one"},
        {"labels", candidates},
    };
    return lemon::make_classifier(cfg);
}

static void test_classifier_structured_choice() {
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("router-model",
        "{\"model\": \"Qwen3.5-35B-A3B-GGUF\", \"rationale\": \"Hard reasoning task\"}");
    ClassifierServices svc = fake.make();

    auto llm = make_llm({"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"});
    Score s = llm->evaluate(ClassifierContext{make_route("prove this theorem"), svc});

    check("llm: structured choice scores chosen candidate 1.0",
          s.ok && s.labels.size() == 1 && s.score_of("Qwen3.5-35B-A3B-GGUF") == 1.0);
    check("llm: rationale records the model's stated reason, not the name",
          s.rationale == "Hard reasoning task");
}

// The router LLM receives a structured JSON view of the routing context —
// not just the latest text — so image-only, tool-bearing, and metadata-tagged
// requests are all visible to it. History policy: text is the latest user
// turn only (the frozen v1 RouteContext contract).
static void test_classifier_receives_structured_context() {
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("router-model",
        "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"ok\"}");
    ClassifierServices svc = fake.make();
    std::string seen_input;
    auto inner = svc.chat;
    svc.chat = [&seen_input, inner](const std::string& m, const std::string& p,
                                    const std::string& i) {
        seen_input = i;
        return inner(m, p, i);
    };
    auto llm = make_llm({"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"});

    // Image-only request: empty text, has_images=true — must not look empty.
    RouteContext image_only;
    image_only.input = "";
    image_only.params.has_images = true;
    image_only.params.chars = 0;
    Score s1 = llm->evaluate(ClassifierContext{image_only, svc});
    json payload = json::parse(seen_input);
    check("llm: image-only request is visible as has_images with empty text",
          payload.value("has_images", false) && payload.value("text", "x").empty());
    check("llm: image-only request still routes via the structured payload",
          s1.ok && s1.score_of("Qwen3-8B-GGUF") == 1.0);

    // Tool-bearing request with no textual tool hint.
    RouteContext with_tools;
    with_tools.input = "do it";
    with_tools.params.has_tools = true;
    with_tools.params.chars = 5;
    llm->evaluate(ClassifierContext{with_tools, svc});
    payload = json::parse(seen_input);
    check("llm: tool availability is visible without a textual hint",
          payload.value("has_tools", false) && payload.value("text", "") == "do it");
    check("llm: chars is carried in the payload",
          payload.value("chars", 0) == 5);

    // Metadata routing hints are passed verbatim.
    RouteContext with_meta;
    with_meta.input = "hi";
    with_meta.metadata["task_class"] = "support";
    llm->evaluate(ClassifierContext{with_meta, svc});
    payload = json::parse(seen_input);
    check("llm: metadata routing hints reach the router",
          payload["metadata"].value("task_class", "") == "support");
}

static void test_classifier_prompt_carries_contract() {
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("router-model",
        "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"ok\"}");
    ClassifierServices svc = fake.make();
    std::string seen_prompt;
    auto inner = svc.chat;
    svc.chat = [&seen_prompt, inner](const std::string& m, const std::string& p,
                                     const std::string& i) {
        seen_prompt = p;
        return inner(m, p, i);
    };

    auto llm = make_llm({"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"});
    llm->evaluate(ClassifierContext{make_route("hi"), svc});

    check("llm: composed prompt keeps the author text",
          seen_prompt.rfind("pick one", 0) == 0);
    check("llm: composed prompt lists every candidate",
          seen_prompt.find("Qwen3-8B-GGUF") != std::string::npos &&
          seen_prompt.find("Qwen3.5-35B-A3B-GGUF") != std::string::npos);
    check("llm: composed prompt states the JSON response contract",
          seen_prompt.find("\"model\"") != std::string::npos &&
          seen_prompt.find("\"rationale\"") != std::string::npos);
    check("llm: composed prompt describes the structured input and history policy",
          seen_prompt.find("latest user turn only") != std::string::npos);
}

static void test_classifier_fenced_json_is_tolerated() {
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("router-model",
        "```json\n{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"Small ask\"}\n```");
    ClassifierServices svc = fake.make();

    auto llm = make_llm({"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"});
    Score s = llm->evaluate(ClassifierContext{make_route("hi"), svc});

    check("llm: one fenced JSON block is stripped and parsed",
          s.ok && s.score_of("Qwen3-8B-GGUF") == 1.0 && s.rationale == "Small ask");
}

// Review point: substring matching can turn an invalid or contradictory answer
// into a valid route. Only an exact trimmed candidate name may match; every
// reply below must yield an empty score (=> fail-open to default_model).
static void test_classifier_rejects_non_exact_replies() {
    const std::vector<std::string> candidates = {"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"};
    struct Case { const char* name; const char* reply; };
    const Case cases[] = {
        {"llm: prose surrounding a candidate is rejected (malformed)",
         "I would use Qwen3-8B-GGUF for this."},
        {"llm: bare candidate name without JSON is rejected (malformed)",
         "Qwen3-8B-GGUF"},
        {"llm: negated/contradictory prose is rejected (malformed)",
         "Do not use Qwen3.5-35B-A3B-GGUF. Use gpt-4o instead."},
        {"llm: JSON naming an unknown model is rejected",
         "{\"model\": \"gpt-4o\", \"rationale\": \"whatever\"}"},
        {"llm: JSON missing the model field is rejected",
         "{\"rationale\": \"no choice made\"}"},
        {"llm: JSON missing the rationale field is rejected",
         "{\"model\": \"Qwen3-8B-GGUF\"}"},
        {"llm: JSON with empty rationale is rejected",
         "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"\"}"},
        {"llm: JSON with whitespace-only rationale is rejected",
         "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"   \"}"},
        {"llm: JSON with non-string rationale is rejected",
         "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": 42}"},
        {"llm: JSON with extra properties is rejected",
         "{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"ok\", \"confidence\": 0.9}"},
        {"llm: fenced JSON followed by trailing prose is rejected",
         "```json\n{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"ok\"}\n```\nHope that helps!"},
        {"llm: opening fence without a closing fence is rejected",
         "```json\n{\"model\": \"Qwen3-8B-GGUF\", \"rationale\": \"ok\"}"},
    };
    for (const Case& c : cases) {
        lemon::testing::FakeClassifierServices fake;
        fake.set_chat_reply("router-model", c.reply);
        ClassifierServices svc = fake.make();
        auto llm = make_llm(candidates);
        Score s = llm->evaluate(ClassifierContext{make_route("hi"), svc});
        check(c.name, s.ok && s.labels.empty());
    }
}

static void test_classifier_backend_failure() {
    // A chat seam that throws must surface as Score::ok=false, not an exception.
    ClassifierServices svc;
    svc.chat = [](const std::string&, const std::string&, const std::string&) -> std::string {
        throw std::runtime_error("backend down");
    };
    auto llm = make_llm({"Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"});
    Score s = llm->evaluate(ClassifierContext{make_route("hi"), svc});
    check("llm: backend failure yields Score::ok=false", !s.ok);
}

// ---------------------------------------------------------------------------
// Parser desugaring
// ---------------------------------------------------------------------------

static void test_desugar_shape() {
    RoutePolicy policy = parse_l0a(l0a_collection());

    check("desugar: candidates preserved",
          policy.candidates.size() == 2 &&
              policy.candidates[0] == "Qwen3-8B-GGUF" &&
              policy.candidates[1] == "Qwen3.5-35B-A3B-GGUF");
    check("desugar: default_model preserved", policy.default_model == "Qwen3-8B-GGUF");
    check("desugar: exactly one synthesized llm classifier",
          policy.classifiers.size() == 1 && policy.classifiers.count("__router") == 1 &&
              policy.classifiers.at("__router")->type() == "llm");
    check("desugar: one identity rule per candidate",
          policy.rules.size() == 2 &&
              policy.rules[0].route_to == "Qwen3-8B-GGUF" &&
              policy.rules[1].route_to == "Qwen3.5-35B-A3B-GGUF");
}

static void test_desugar_rejects_router_plus_rules() {
    json bad = l0a_collection();
    bad["routing"]["rules"] = json::array({json{
        {"id", "r0"}, {"match", {{"keywords_any", {"x"}}}}, {"route_to", "Qwen3-8B-GGUF"}}});
    bool threw = false;
    try { parse_l0a(bad); } catch (const std::invalid_argument&) { threw = true; }
    check("desugar: router + explicit rules is rejected", threw);
}

// ---------------------------------------------------------------------------
// End-to-end engine routing (the acceptance path)
// ---------------------------------------------------------------------------

static Decision route_with_reply(const std::string& reply, const std::string& input) {
    RoutePolicy policy = parse_l0a(l0a_collection());
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("Qwen3-1.7B-GGUF", reply);  // the router's own model
    RoutingPolicyEngine engine(std::move(policy), fake.make());
    return engine.route(make_route(input), /*want_trace=*/true);
}

static void test_e2e_routes_to_chosen() {
    Decision d = route_with_reply(
        "{\"model\": \"Qwen3.5-35B-A3B-GGUF\", \"rationale\": \"Hard reasoning\"}",
        "solve this hard problem");
    check("e2e: routes to the LLM-chosen candidate",
          d.route_to == "Qwen3.5-35B-A3B-GGUF" && !d.default_used);

    bool trace_has_rationale = false;
    bool rejected_entry_clean = true;
    bool saw_rejected_entry = false;
    for (const auto& e : d.trace) {
        if (e.condition == "classifier:__router" && e.result &&
            e.label == "Qwen3.5-35B-A3B-GGUF" &&
            e.rationale == "Hard reasoning") {
            trace_has_rationale = true;
        }
        // The selected candidate is the SECOND identity rule, so the first
        // band (Qwen3-8B) is evaluated and rejected before it. The winner's
        // rationale must not be attached to that rejected entry.
        if (e.condition == "classifier:__router" && !e.result) {
            saw_rejected_entry = true;
            if (!e.rationale.empty()) rejected_entry_clean = false;
        }
    }
    check("e2e: trace identifies the tested label and records the rationale", trace_has_rationale);
    check("e2e: rejected candidate entries do not carry the winner's rationale",
          saw_rejected_entry && rejected_entry_clean);
}

static void test_e2e_invalid_falls_back() {
    Decision d = route_with_reply(
        "{\"model\": \"some-unknown-model\", \"rationale\": \"n/a\"}", "hello");
    check("e2e: unknown structured choice falls back to default_model",
          d.route_to == "Qwen3-8B-GGUF" && d.default_used);

    Decision d2 = route_with_reply("I think the small model is fine here.", "hello");
    check("e2e: malformed (non-JSON) reply falls back to default_model",
          d2.route_to == "Qwen3-8B-GGUF" && d2.default_used);
}

int main() {
    test_classifier_structured_choice();
    test_classifier_receives_structured_context();
    test_classifier_prompt_carries_contract();
    test_classifier_fenced_json_is_tolerated();
    test_classifier_rejects_non_exact_replies();
    test_classifier_backend_failure();
    test_desugar_shape();
    test_desugar_rejects_router_plus_rules();
    test_e2e_routes_to_chosen();
    test_e2e_invalid_falls_back();

    if (g_failures == 0) {
        std::printf("All llm router tests passed.\n");
    } else {
        std::printf("%d llm router test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
