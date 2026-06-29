// Unit tests for the Lemonade Router match evaluator (#2378).
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       test/cpp/test_routing_policy_evaluator.cpp src/cpp/server/routing_policy.cpp \
//       -o test_routing_policy_evaluator

#include "lemon/routing_policy.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::Classifier;
using lemon::ClassifierContext;
using lemon::Condition;
using lemon::ConditionPtr;
using lemon::EvalContext;
using lemon::LeafFactory;
using lemon::MatchExpr;
using lemon::OnError;
using lemon::RouteContext;
using lemon::Score;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

namespace {

struct CountingCondition : Condition {
    bool value;
    int* calls;
    std::string trace_name;

    CountingCondition(bool v, int* c, std::string name = "counting")
        : value(v), calls(c), trace_name(std::move(name)) {}

    bool evaluate(EvalContext& ctx) const override {
        ++(*calls);
        if (ctx.want_trace) {
            ctx.trace.push_back(lemon::TraceEntry{trace_name, std::nullopt, value});
        }
        return value;
    }
};

struct FixedClassifier : Classifier {
    mutable int calls = 0;
    Score score;

    FixedClassifier(std::string id, OnError on_error, Score s,
                    std::vector<std::string> labels = {},
                    std::optional<std::string> default_label = std::nullopt)
        : Classifier(std::move(id), "classifier", on_error, std::move(labels),
                     std::move(default_label)),
          score(std::move(s)) {}

    Score evaluate(const ClassifierContext&) const override {
        ++calls;
        return score;
    }
};

struct ThrowingClassifier : Classifier {
    mutable int calls = 0;

    ThrowingClassifier(std::string id, OnError on_error)
        : Classifier(std::move(id), "classifier", on_error) {}

    Score evaluate(const ClassifierContext&) const override {
        ++calls;
        throw std::runtime_error("boom");
    }
};

} // namespace

static EvalContext make_eval_context(bool want_trace = false) {
    static RouteContext route;
    static lemon::ClassifierServices services;
    route.input = "hello";
    route.params.model = "user.Router";
    route.params.chars = route.input.size();
    EvalContext ctx{route, services};
    ctx.want_trace = want_trace;
    return ctx;
}

static void test_composite_short_circuit() {
    int a = 0, b = 0;
    EvalContext ctx = make_eval_context();
    auto all = lemon::make_all_condition({
        std::make_shared<CountingCondition>(false, &a),
        std::make_shared<CountingCondition>(true, &b),
    });
    check("all short-circuits on first false", !all->evaluate(ctx) && a == 1 && b == 0);

    a = 0;
    b = 0;
    auto any = lemon::make_any_condition({
        std::make_shared<CountingCondition>(true, &a),
        std::make_shared<CountingCondition>(false, &b),
    });
    check("any short-circuits on first true", any->evaluate(ctx) && a == 1 && b == 0);
}

static void test_not_and_deep_compile() {
    int calls = 0;
    LeafFactory factory = [&](const json& leaf) -> ConditionPtr {
        return std::make_shared<CountingCondition>(leaf.value("value", false), &calls);
    };

    MatchExpr leaf_false;
    leaf_false.op = MatchExpr::Op::Leaf;
    leaf_false.leaf = json{{"value", false}};
    MatchExpr not_false;
    not_false.op = MatchExpr::Op::Not;
    not_false.children = {leaf_false};

    EvalContext ctx = make_eval_context();
    auto not_condition = lemon::compile_match_expr(not_false, factory);
    check("not inverts child result", not_condition->evaluate(ctx) && calls == 1);

    MatchExpr leaf_true;
    leaf_true.op = MatchExpr::Op::Leaf;
    leaf_true.leaf = json{{"value", true}};
    MatchExpr any;
    any.op = MatchExpr::Op::Any;
    any.children = {leaf_false, leaf_true};
    MatchExpr all;
    all.op = MatchExpr::Op::All;
    all.children = {not_false, any};

    calls = 0;
    auto deep = lemon::compile_match_expr(all, factory);
    check("deep nested compile/evaluate succeeds", deep->evaluate(ctx) && calls == 3);
}

static void test_compile_rejects_malformed_ast() {
    static int unused_calls = 0;
    LeafFactory factory = [](const json&) -> ConditionPtr {
        return std::make_shared<CountingCondition>(true, &unused_calls);
    };

    bool threw_any = false;
    try {
        MatchExpr bad;
        bad.op = MatchExpr::Op::Any;
        lemon::compile_match_expr(bad, factory);
    } catch (const std::invalid_argument&) {
        threw_any = true;
    }
    check("compile rejects empty any", threw_any);

    bool threw_not = false;
    try {
        MatchExpr bad;
        bad.op = MatchExpr::Op::Not;
        bad.children = {MatchExpr{}, MatchExpr{}};
        lemon::compile_match_expr(bad, factory);
    } catch (const std::invalid_argument&) {
        threw_not = true;
    }
    check("compile rejects not with multiple children", threw_not);
}

static void test_compile_rejects_excessive_depth() {
    LeafFactory factory = [](const json&) -> ConditionPtr {
        static int calls = 0;
        return std::make_shared<CountingCondition>(true, &calls);
    };

    MatchExpr leaf;
    leaf.op = MatchExpr::Op::Leaf;
    leaf.leaf = json{{"value", true}};

    MatchExpr nested = leaf;
    for (int i = 0; i < 70; ++i) {
        MatchExpr parent;
        parent.op = MatchExpr::Op::Not;
        parent.children = {nested};
        nested = parent;
    }

    bool threw = false;
    try {
        lemon::compile_match_expr(nested, factory);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("compile rejects excessive match depth", threw);
}

static void test_classifier_band_boundaries_and_defaults() {
    Score s;
    s.labels["PII"] = 0.5;
    auto classifier = std::make_shared<FixedClassifier>(
        "pii", OnError::MatchFalse, s, std::vector<std::string>{"PII"}, std::string("PII"));

    EvalContext ctx = make_eval_context();
    auto min_ok = lemon::make_classifier_band_condition(classifier, std::nullopt, 0.5, std::nullopt);
    check("classifier min_score boundary is inclusive", min_ok->evaluate(ctx));

    EvalContext ctx2 = make_eval_context();
    auto max_ok = lemon::make_classifier_band_condition(classifier, std::string("PII"), std::nullopt, 0.5);
    check("classifier max_score boundary is inclusive", max_ok->evaluate(ctx2));

    EvalContext ctx3 = make_eval_context();
    auto default_min = lemon::make_classifier_band_condition(classifier, std::nullopt, std::nullopt, std::nullopt);
    check("classifier omitted band defaults to min_score 0.5", default_min->evaluate(ctx3));

    Score low;
    low.labels["PII"] = 0.49;
    auto low_classifier = std::make_shared<FixedClassifier>(
        "low", OnError::MatchFalse, low, std::vector<std::string>{"PII"}, std::string("PII"));
    EvalContext ctx4 = make_eval_context();
    auto low_default = lemon::make_classifier_band_condition(low_classifier, std::nullopt, std::nullopt, std::nullopt);
    check("classifier default min_score rejects below 0.5", !low_default->evaluate(ctx4));
}

static void test_classifier_score_selection_and_primary() {
    Score labeled;
    labeled.labels["PII"] = 0.1;
    labeled.labels["NO_PII"] = 0.9;
    auto classifier = std::make_shared<FixedClassifier>(
        "pii", OnError::MatchFalse, labeled, std::vector<std::string>{"PII", "NO_PII"},
        std::string("NO_PII"));

    EvalContext ctx = make_eval_context();
    auto condition = lemon::make_classifier_band_condition(classifier, std::nullopt, 0.8, std::nullopt);
    check("classifier condition uses default_label when label omitted", condition->evaluate(ctx));

    Score single;
    single.labels[""] = 0.72;
    auto similarity = std::make_shared<FixedClassifier>(
        "sim", OnError::MatchFalse, single);
    EvalContext ctx2 = make_eval_context();
    auto primary = lemon::make_classifier_band_condition(similarity, std::nullopt, 0.7, std::nullopt);
    check("classifier condition falls back to Score::primary", primary->evaluate(ctx2));

    Score missing;
    missing.labels["NO_PII"] = 0.99;
    auto missing_label_classifier = std::make_shared<FixedClassifier>(
        "missing", OnError::MatchFalse, missing,
        std::vector<std::string>{"PII", "NO_PII"}, std::string("PII"));
    EvalContext ctx3 = make_eval_context();
    auto missing_label = lemon::make_classifier_band_condition(
        missing_label_classifier, std::nullopt, 0.5, std::nullopt);
    check("missing classifier output label scores as 0", !missing_label->evaluate(ctx3));
}

static void test_classifier_on_error_and_throwing() {
    Score failed;
    failed.ok = false;

    auto fail_true = std::make_shared<FixedClassifier>("fail_true", OnError::MatchTrue, failed);
    EvalContext ctx = make_eval_context();
    auto true_condition = lemon::make_classifier_band_condition(fail_true, std::nullopt, 0.5, std::nullopt);
    check("classifier on_error match_true returns true", true_condition->evaluate(ctx));

    auto fail_false = std::make_shared<FixedClassifier>("fail_false", OnError::MatchFalse, failed);
    EvalContext ctx2 = make_eval_context();
    auto false_condition = lemon::make_classifier_band_condition(fail_false, std::nullopt, 0.5, std::nullopt);
    check("classifier on_error match_false returns false", !false_condition->evaluate(ctx2));

    auto throwing = std::make_shared<ThrowingClassifier>("throwing", OnError::MatchTrue);
    EvalContext ctx3 = make_eval_context();
    auto throwing_condition = lemon::make_classifier_band_condition(throwing, std::nullopt, 0.5, std::nullopt);
    check("classifier exceptions become on_error decisions", throwing_condition->evaluate(ctx3));
}

static void test_classifier_memoization_and_trace() {
    Score s;
    s.labels["PII"] = 0.9;
    auto classifier = std::make_shared<FixedClassifier>(
        "pii", OnError::MatchFalse, s, std::vector<std::string>{"PII"}, std::string("PII"));

    auto left = lemon::make_classifier_band_condition(classifier, std::nullopt, 0.5, std::nullopt);
    auto right = lemon::make_classifier_band_condition(classifier, std::string("PII"), 0.8, std::nullopt);
    auto all = lemon::make_all_condition({left, right});

    EvalContext ctx = make_eval_context(true);
    check("classifier memoized across multiple conditions", all->evaluate(ctx) && classifier->calls == 1);
    check("trace includes evaluated classifier leaves", ctx.trace.size() == 2 &&
              ctx.trace[0].condition == "classifier:pii" &&
              ctx.trace[0].score.has_value() && *ctx.trace[0].score == 0.9);

    int skipped_calls = 0;
    auto short_circuit = lemon::make_any_condition({
        std::make_shared<CountingCondition>(true, &skipped_calls, "first"),
        std::make_shared<CountingCondition>(true, &skipped_calls, "second"),
    });
    EvalContext trace_ctx = make_eval_context(true);
    check("trace omits short-circuited branches", short_circuit->evaluate(trace_ctx) &&
              skipped_calls == 1 && trace_ctx.trace.size() == 1 &&
              trace_ctx.trace[0].condition == "first");

    EvalContext no_trace_ctx = make_eval_context(false);
    check("trace disabled leaves trace empty", left->evaluate(no_trace_ctx) &&
              no_trace_ctx.trace.empty());
}

static void test_classifier_band_rejects_bad_band() {
    auto classifier = std::make_shared<FixedClassifier>("pii", OnError::MatchFalse, Score{});
    bool threw = false;
    try {
        lemon::make_classifier_band_condition(classifier, std::nullopt, 0.9, 0.1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("classifier band rejects min_score > max_score", threw);
}

int main() {
    test_composite_short_circuit();
    test_not_and_deep_compile();
    test_compile_rejects_malformed_ast();
    test_compile_rejects_excessive_depth();
    test_classifier_band_boundaries_and_defaults();
    test_classifier_score_selection_and_primary();
    test_classifier_on_error_and_throwing();
    test_classifier_memoization_and_trace();
    test_classifier_band_rejects_bad_band();

    std::printf("\n%s\n", g_failures == 0 ? "ALL EVALUATOR TESTS PASSED"
                                          : "EVALUATOR TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
