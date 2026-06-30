// Foundation contract test for the generic routing engine.
//
// This test asserts the CONTRACT SURFACE only — there is no engine behavior yet
// (the evaluator, registry, and assembly are implemented separately). It proves:
//   1. routing_policy.h compiles standalone with no backend/Router include.
//   2. Every contract type constructs and round-trips its fields.
//   3. The fake ClassifierServices satisfies the injection seam and is callable.
//   4. RoutingPolicyEngine is constructible (route() is intentionally not called).
//   5. The committed L0a-L3 fixtures parse and satisfy the locked structural
//      invariants (candidates non-empty; default_model and every route_to are
//      candidates; classifier condition refs resolve; router XOR rules present).
//      Full JSON-Schema validation lives in the Python test
//      test/test_routing_fixtures.py.
//
// Compile (standalone):
//   cl /std:c++17 /EHsc /I src/cpp/include /I build/_deps/json-src/include \
//      /DROUTING_FIXTURE_DIR=... test/cpp/test_routing_policy_contract.cpp

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef ROUTING_FIXTURE_DIR
#define ROUTING_FIXTURE_DIR "test/cpp/fixtures/routing"
#endif

using lemon::Classifier;
using lemon::ClassifierContext;
using lemon::ClassifierServices;
using lemon::Condition;
using lemon::ConditionPtr;
using lemon::Decision;
using lemon::EvalContext;
using lemon::LeafFactory;
using lemon::MatchExpr;
using lemon::OnError;
using lemon::RouteContext;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::Rule;
using lemon::Score;
using lemon::TraceEntry;
using lemon::json;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

static json load_json(const std::string& name) {
    std::string path = std::string(ROUTING_FIXTURE_DIR) + "/" + name;
    std::ifstream in(path);
    if (!in) {
        std::printf("[FAIL] could not open fixture %s\n", path.c_str());
        ++g_failures;
        return json::object();
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
}

// ---------------------------------------------------------------------------
// 1-2. Contract types construct and carry their fields.
// ---------------------------------------------------------------------------
static void test_types_construct() {
    RouteContext ctx;
    ctx.input = "write a function to reverse a list";
    ctx.params.model = "user.Router-Keywords";
    ctx.params.has_tools = true;
    ctx.params.has_images = false;
    ctx.params.chars = ctx.input.size();
    ctx.metadata["task_class"] = "payment";
    check(ctx.metadata.at("task_class") == "payment", "RouteContext carries metadata");
    check(ctx.params.chars == ctx.input.size(), "RouteContext params carry char count");

    // Score helpers (the label->score contract).
    Score s;
    s.labels["PII"] = 0.81;
    s.labels["NO_PII"] = 0.19;
    check(s.ok, "Score defaults ok=true");
    check(s.score_of("PII") == 0.81, "Score::score_of returns the labeled score");
    check(s.score_of("missing") == 0.0, "Score::score_of returns 0 for absent label");

    Score single;
    single.labels["POSITIVE"] = 0.73;  // a one-label classifier score
    check(single.primary() == 0.73, "Score::primary reads the lone entry");

    Score multi;
    multi.labels["NEGATIVE"] = 0.30;
    multi.labels["POSITIVE"] = 0.70;
    check(multi.primary() == 0.0,
          "Score::primary returns 0.0 for a multi-label score (no lone entry)");

    Score none;
    check(none.primary() == 0.0, "Score::primary returns 0.0 for an empty score");

    // on_error round-trips through the single-source-of-truth mapping.
    check(lemon::parse_on_error("match_true") == OnError::MatchTrue, "parse_on_error match_true");
    check(lemon::parse_on_error("match_false") == OnError::MatchFalse, "parse_on_error match_false");
    check(std::string(lemon::on_error_to_string(OnError::MatchTrue)) == "match_true",
          "on_error_to_string round-trips");

    // Match AST: any[ leaf, not(leaf) ].
    MatchExpr leaf;
    leaf.op = MatchExpr::Op::Leaf;
    leaf.leaf = json{{"keywords_any", {"def ", "function"}}};
    MatchExpr neg;
    neg.op = MatchExpr::Op::Not;
    neg.children.push_back(leaf);
    MatchExpr expr;
    expr.op = MatchExpr::Op::Any;
    expr.children = {leaf, neg};
    check(expr.children.size() == 2, "MatchExpr nests children");
    check(expr.children[1].op == MatchExpr::Op::Not, "MatchExpr carries Not node");

    Rule rule;
    rule.id = "code-to-big";
    rule.match = expr;
    rule.route_to = "vllm.qwen3-32b";
    rule.outputs = json{{"verdict", "warn"}};
    check(rule.outputs.at("verdict") == "warn", "Rule carries opaque outputs bag");

    // Decision + trace shape.
    Decision d;
    d.route_to = "Qwen3-8B-GGUF";
    d.matched_rule = "keep-private";
    d.default_used = false;
    d.outputs = json{{"verdict", "warn"}};
    d.trace.push_back(TraceEntry{"classifier:pii", 0.81, true});
    d.trace.push_back(TraceEntry{"keywords_any", std::nullopt, false});
    check(d.trace.size() == 2, "Decision carries trace entries");
    check(d.trace[0].score.has_value() && *d.trace[0].score == 0.81,
          "TraceEntry carries optional classifier score");
    check(!d.trace[1].score.has_value(), "TraceEntry score is absent for deterministic leaf");
}

// ---------------------------------------------------------------------------
// 2b. The Condition / EvalContext / LeafFactory evaluation seam.
// ---------------------------------------------------------------------------
namespace {
// A trivial leaf Condition: stands in for the deterministic/classifier-band
// conditions built downstream, proving the interface is implementable here.
struct ConstCondition : Condition {
    bool value;
    explicit ConstCondition(bool v) : value(v) {}
    bool evaluate(EvalContext& ctx) const override {
        if (ctx.want_trace) ctx.trace.push_back(TraceEntry{"const", std::nullopt, value});
        return value;
    }
};

// A model-backed classifier: declares labels + a default, as the registry reads
// them to resolve condition `label` refs.
struct LabeledClassifier : Classifier {
    LabeledClassifier()
        : Classifier("pii", "classifier", OnError::MatchTrue,
                     {"PII", "NO_PII"}, std::string("PII")) {}
    Score evaluate(const ClassifierContext&) const override {
        Score s;
        s.labels["PII"] = 1.0;
        return s;
    }
};

// A label-less classifier: declares no labels and returns a single score, read
// via primary().
struct SingleScoreClassifier : Classifier {
    SingleScoreClassifier() : Classifier("single", "classifier", OnError::MatchFalse) {}
    Score evaluate(const ClassifierContext&) const override {
        Score s;
        s.labels["POSITIVE"] = 0.5;
        return s;
    }
};
} // namespace

static void test_classifier_contract() {
    LabeledClassifier lc;
    check(lc.id() == "pii" && lc.type() == "classifier", "Classifier carries id/type");
    check(lc.on_error() == OnError::MatchTrue, "Classifier carries on_error");
    check(lc.labels().size() == 2 && lc.labels()[0] == "PII",
          "Classifier exposes declared labels for ref resolution");
    check(lc.default_label().has_value() && *lc.default_label() == "PII",
          "Classifier exposes default_label");

    SingleScoreClassifier sc;
    check(sc.labels().empty() && !sc.default_label().has_value(),
          "a label-less classifier declares no labels (read via primary())");
}

static void test_condition_seam() {
    RouteContext ctx;
    ctx.input = "hi";
    lemon::testing::FakeClassifierServices fake;
    ClassifierServices svc = fake.make();

    EvalContext ec{ctx, svc};
    ec.want_trace = true;
    ec.memo["pii"] = Score{};  // memo is keyed by classifier id
    check(ec.memo.count("pii") == 1, "EvalContext memo is keyed by classifier id");

    ConditionPtr cond = std::make_shared<ConstCondition>(true);
    check(cond->evaluate(ec), "Condition::evaluate is callable through the interface");
    check(ec.trace.size() == 1 && ec.trace[0].condition == "const",
          "a Condition appends to EvalContext::trace when want_trace");

    LeafFactory factory = [](const json& leaf) -> ConditionPtr {
        return std::make_shared<ConstCondition>(leaf.value("v", false));
    };
    ConditionPtr built = factory(json{{"v", true}});
    check(static_cast<bool>(built) && built->evaluate(ec),
          "LeafFactory builds a leaf Condition from leaf JSON");
}

// ---------------------------------------------------------------------------
// 3. The fake ClassifierServices satisfies the injection seam.
// ---------------------------------------------------------------------------
static void test_fake_services() {
    lemon::testing::FakeClassifierServices fake;
    fake.set_embedding("nomic-embed-text-v1.5-GGUF", {0.0f, 1.0f, 0.0f});
    fake.set_classifier_scores("pii-detector-small", {{"PII", 0.9}, {"NO_PII", 0.1}});
    fake.set_chat_reply("Qwen3-1.7B-GGUF", "Qwen3.5-35B-A3B-GGUF");

    ClassifierServices svc = fake.make();
    check(static_cast<bool>(svc.embed) && static_cast<bool>(svc.run_classifier) &&
              static_cast<bool>(svc.chat),
          "ClassifierServices exposes embed/run_classifier/chat");

    auto vec = svc.embed("nomic-embed-text-v1.5-GGUF", "anything");
    check(vec.size() == 3 && vec[1] == 1.0f, "fake embed returns configured vector");

    auto scores = svc.run_classifier("pii-detector-small", "my ssn is ...");
    check(scores.at("PII") == 0.9, "fake run_classifier returns configured scores");

    auto reply = svc.chat("Qwen3-1.7B-GGUF", "route this", "hard reasoning task");
    check(reply == "Qwen3.5-35B-A3B-GGUF", "fake chat returns configured reply");
}

// ---------------------------------------------------------------------------
// 4. The engine is constructible against the contract (route() not called).
// ---------------------------------------------------------------------------
static void test_engine_constructs() {
    RoutePolicy policy;
    policy.candidates = {"Qwen3-8B-GGUF", "vllm.qwen3-32b"};
    policy.default_model = "Qwen3-8B-GGUF";

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyEngine engine(std::move(policy), fake.make());
    check(engine.policy().candidates.size() == 2, "RoutingPolicyEngine is constructible");
    check(engine.policy().default_model == "Qwen3-8B-GGUF", "engine exposes its policy");
}

// ---------------------------------------------------------------------------
// 5. Fixtures parse and satisfy the locked structural invariants.
// ---------------------------------------------------------------------------
static void validate_fixture(const std::string& name) {
    json doc = load_json(name);
    std::string tag = "fixture " + name + ":";

    if (!doc.is_object() || !doc.contains("routing")) {
        check(false, (tag + " parses with a routing block").c_str());
        return;
    }
    check(doc.value("version", "") == "1", (tag + " declares schema version 1").c_str());
    check(doc.value("recipe", "") == "collection.router",
          (tag + " recipe is collection.router").c_str());

    const json& routing = doc["routing"];
    std::set<std::string> candidates;
    for (const auto& c : routing.value("candidates", json::array())) {
        candidates.insert(c.get<std::string>());
    }
    check(!candidates.empty(), (tag + " candidates is non-empty").c_str());

    const std::string default_model = routing.value("default_model", std::string{});
    check(candidates.count(default_model) == 1,
          (tag + " default_model is a candidate").c_str());

    // router XOR rules (the anyOf in the schema; lean local form uses one).
    const bool has_router = routing.contains("router");
    const bool has_rules = routing.contains("rules");
    check(has_router || has_rules, (tag + " declares router or rules").c_str());

    // Every rule.route_to is a candidate.
    for (const auto& rule : routing.value("rules", json::array())) {
        const std::string route_to = rule.value("route_to", std::string{});
        check(candidates.count(route_to) == 1,
              (tag + " rule '" + rule.value("id", std::string{}) +
               "' routes to a candidate").c_str());
    }

    // Classifier condition refs resolve against declared classifier ids.
    std::set<std::string> classifier_ids;
    for (const auto& c : routing.value("classifiers", json::array())) {
        classifier_ids.insert(c.value("id", std::string{}));
    }
    std::function<void(const json&)> check_refs = [&](const json& expr) {
        if (!expr.is_object()) return;
        if (expr.contains("classifier")) {
            check(classifier_ids.count(expr["classifier"].get<std::string>()) == 1,
                  (tag + " classifier ref '" +
                   expr["classifier"].get<std::string>() + "' resolves").c_str());
        }
        for (const char* op : {"any", "all"}) {
            if (expr.contains(op)) {
                for (const auto& child : expr[op]) check_refs(child);
            }
        }
        if (expr.contains("not")) check_refs(expr["not"]);
    };
    for (const auto& rule : routing.value("rules", json::array())) {
        if (rule.contains("match")) check_refs(rule["match"]);
    }
}

static void test_fixtures() {
    for (const char* name : {"l0a_llm_router.json", "l1_keywords.json",
                             "l1_metadata.json", "l2_semantic.json",
                             "l3_classifier.json"}) {
        validate_fixture(name);
    }
    // Decision example parses and carries the locked keys.
    json dec = load_json("decision_example.json");
    check(dec.value("version", "") == "1" && dec.contains("route_to") &&
              dec.contains("matched_rule") && dec.contains("default_used") &&
              dec.contains("trace"),
          "decision_example.json carries version/route_to/matched_rule/default_used/trace");

    // Request extension example: metadata is a string map, route_trace a bool.
    json req = load_json("request_example.json");
    json md = req.contains("metadata") ? req["metadata"] : json::object();
    bool meta_strings = req.contains("metadata") && md.is_object();
    for (auto& kv : md.items()) {
        if (!kv.value().is_string()) meta_strings = false;
    }
    check(meta_strings && req.value("route_trace", false) == true,
          "request_example.json carries string-valued metadata and route_trace");
}

int main() {
    test_types_construct();
    test_condition_seam();
    test_classifier_contract();
    test_fake_services();
    test_engine_constructs();
    test_fixtures();
    std::printf("\n%s\n", g_failures == 0 ? "ALL CONTRACT CHECKS PASSED"
                                          : "CONTRACT CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
