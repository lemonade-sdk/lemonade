// Unit tests for the Lemonade Router classifier/leaf registry (#2379).

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"

#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

using lemon::Condition;
using lemon::ConditionPtr;
using lemon::EvalContext;
using lemon::NamedLeafFactories;
using lemon::RouteContext;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

namespace {

struct ConstCondition : Condition {
    bool value;
    int* calls;
    std::string trace_name;

    ConstCondition(bool v, int* c, std::string name)
        : value(v), calls(c), trace_name(std::move(name)) {}

    bool evaluate(EvalContext& ctx) const override {
        ++(*calls);
        if (ctx.want_trace) ctx.trace.push_back(lemon::TraceEntry{trace_name, std::nullopt, value});
        return value;
    }
};

} // namespace

static RouteContext make_route_context() {
    RouteContext route;
    route.input = "my ssn is 123";
    route.params.model = "user.Router";
    route.params.chars = route.input.size();
    return route;
}

static EvalContext make_eval_context(const RouteContext& route,
                                     const lemon::ClassifierServices& services,
                                     bool want_trace = false) {
    EvalContext ctx{route, services};
    ctx.want_trace = want_trace;
    return ctx;
}

static bool throws_invalid_arg(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

static void test_make_classifier() {
    json cfg = {
        {"id", "pii"},
        {"type", "classifier"},
        {"model", "pii-detector-small"},
        {"labels", json::array({"PII", "NO_PII"})},
        {"default_label", "PII"},
        {"on_error", "match_true"},
    };
    auto classifier = lemon::make_classifier(cfg);
    check("make_classifier builds generic classifier", classifier->id() == "pii" &&
              classifier->type() == "classifier");
    check("make_classifier preserves labels/default/on_error",
          classifier->labels().size() == 2 &&
              classifier->default_label().has_value() &&
              *classifier->default_label() == "PII" &&
              classifier->on_error() == lemon::OnError::MatchTrue);

    lemon::testing::FakeClassifierServices fake;
    fake.set_classifier_scores("pii-detector-small", {{"PII", 0.91}, {"NO_PII", 0.09}});
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services);
    auto score = classifier->evaluate(lemon::ClassifierContext{ctx.request, ctx.services});
    check("generic classifier calls run_classifier service", score.ok && score.score_of("PII") == 0.91);
}

static void test_make_classifier_rejections() {
    check("make_classifier rejects missing id",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"type", "classifier"}, {"model", "m"}});
          }));
    check("make_classifier rejects missing type",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"model", "m"}});
          }));
    check("make_classifier rejects missing model for classifier",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "classifier"}});
          }));
    check("make_classifier rejects default_label without labels",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "classifier"},
                                          {"model", "m"}, {"default_label", "PII"}});
          }));
    check("make_classifier rejects default_label not in labels",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "classifier"},
                                          {"model", "m"},
                                          {"labels", json::array({"NO_PII"})},
                                          {"default_label", "PII"}});
          }));
    check("make_classifier rejects unknown type",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "mystery"},
                                          {"model", "m"}});
          }));
    check("make_classifier clearly rejects semantic_similarity for now",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "sim"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", json::array({"return"})}});
          }));
    check("make_classifier clearly rejects llm for now",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "router"}, {"type", "llm"},
                                          {"model", "m"}, {"prompt", "choose"}});
          }));
    check("make_classifier clearly rejects reserved presets for now",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "pii"}, {"type", "pii_detection"},
                                          {"model", "m"}});
          }));
}

static void test_make_classifiers() {
    json configs = json::array({
        json{{"id", "pii"}, {"type", "classifier"}, {"model", "pii-detector-small"}},
        json{{"id", "jailbreak"}, {"type", "classifier"}, {"model", "jailbreak-detector-small"}},
    });
    auto classifiers = lemon::make_classifiers(configs);
    check("make_classifiers builds classifier map", classifiers.size() == 2 &&
              classifiers.count("pii") == 1 && classifiers.count("jailbreak") == 1);

    json dupes = json::array({
        json{{"id", "pii"}, {"type", "classifier"}, {"model", "a"}},
        json{{"id", "pii"}, {"type", "classifier"}, {"model", "b"}},
    });
    check("make_classifiers rejects duplicate ids",
          throws_invalid_arg([&] { lemon::make_classifiers(dupes); }));
}

static void test_leaf_factory_classifier_refs() {
    auto classifier = lemon::make_classifier(json{
        {"id", "pii"},
        {"type", "classifier"},
        {"model", "pii-detector-small"},
        {"labels", json::array({"PII", "NO_PII"})},
        {"default_label", "PII"},
    });
    std::map<std::string, lemon::ClassifierPtr> classifiers = {{"pii", classifier}};
    auto leaf_factory = lemon::make_leaf_factory(classifiers);

    lemon::testing::FakeClassifierServices fake;
    fake.set_classifier_scores("pii-detector-small", {{"PII", 0.88}, {"NO_PII", 0.12}});
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services, true);
    auto condition = leaf_factory(json{{"classifier", "pii"}});
    check("leaf factory uses classifier default_label", condition->evaluate(ctx) &&
              ctx.trace.size() == 1 && ctx.trace[0].score.has_value() &&
              *ctx.trace[0].score == 0.88);

    check("leaf factory rejects dangling classifier ref",
          throws_invalid_arg([&] { leaf_factory(json{{"classifier", "missing"}}); }));
    check("leaf factory rejects dangling label ref",
          throws_invalid_arg([&] {
              leaf_factory(json{{"classifier", "pii"}, {"label", "SECRET"}});
          }));

    auto no_default = lemon::make_classifier(json{
        {"id", "ambiguous"},
        {"type", "classifier"},
        {"model", "pii-detector-small"},
        {"labels", json::array({"PII", "NO_PII"})},
    });
    check("leaf factory rejects omitted label without default_label",
          throws_invalid_arg([&] {
              auto lf = lemon::make_leaf_factory({{"ambiguous", no_default}});
              lf(json{{"classifier", "ambiguous"}});
          }));
}

static void test_leaf_factory_deterministic_dispatch_and_implicit_all() {
    int keywords_calls = 0;
    int chars_calls = 0;
    NamedLeafFactories factories;
    factories["keywords_any"] = [&](const json& leaf) -> ConditionPtr {
        check("deterministic factory receives isolated keywords leaf",
              leaf.size() == 1 && leaf.contains("keywords_any"));
        return std::make_shared<ConstCondition>(true, &keywords_calls, "keywords_any");
    };
    factories["max_chars"] = [&](const json& leaf) -> ConditionPtr {
        check("deterministic factory receives isolated max_chars leaf",
              leaf.size() == 1 && leaf.contains("max_chars"));
        return std::make_shared<ConstCondition>(true, &chars_calls, "max_chars");
    };

    auto leaf_factory = lemon::make_leaf_factory({}, factories);
    auto condition = leaf_factory(json{{"keywords_any", json::array({"return"})}, {"max_chars", 1000}});

    lemon::testing::FakeClassifierServices fake;
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services, true);
    check("multi-key deterministic leaf composes as implicit all", condition->evaluate(ctx) &&
              keywords_calls == 1 && chars_calls == 1 && ctx.trace.size() == 2);

    check("leaf factory rejects unknown leaf",
          throws_invalid_arg([&] { leaf_factory(json{{"unknown", true}}); }));
    check("classifier-only fields require classifier",
          throws_invalid_arg([&] { leaf_factory(json{{"min_score", 0.5}}); }));
}

static void test_leaf_factory_classifier_and_deterministic_implicit_all() {
    auto classifier = lemon::make_classifier(json{
        {"id", "pii"},
        {"type", "classifier"},
        {"model", "pii-detector-small"},
        {"labels", json::array({"PII", "NO_PII"})},
        {"default_label", "PII"},
    });

    int max_chars_calls = 0;
    NamedLeafFactories factories;
    factories["max_chars"] = [&](const json&) -> ConditionPtr {
        return std::make_shared<ConstCondition>(true, &max_chars_calls, "max_chars");
    };

    auto leaf_factory = lemon::make_leaf_factory({{"pii", classifier}}, factories);
    auto condition = leaf_factory(json{{"classifier", "pii"}, {"min_score", 0.5}, {"max_chars", 1000}});

    lemon::testing::FakeClassifierServices fake;
    fake.set_classifier_scores("pii-detector-small", {{"PII", 0.75}, {"NO_PII", 0.25}});
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services, true);
    check("classifier plus deterministic leaf composes as implicit all",
          condition->evaluate(ctx) && max_chars_calls == 1 && ctx.trace.size() == 2);
}

int main() {
    test_make_classifier();
    test_make_classifier_rejections();
    test_make_classifiers();
    test_leaf_factory_classifier_refs();
    test_leaf_factory_deterministic_dispatch_and_implicit_all();
    test_leaf_factory_classifier_and_deterministic_implicit_all();

    std::printf("\n%s\n", g_failures == 0 ? "ALL REGISTRY TESTS PASSED"
                                          : "REGISTRY TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
