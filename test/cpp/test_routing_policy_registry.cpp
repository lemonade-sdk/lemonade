// Unit tests for the Lemonade Router classifier/leaf registry (#2379).

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

static void test_make_zero_shot_classifier() {
    json cfg = {
        {"id", "route"},
        {"type", "zero_shot"},
        {"model", "lfm-prompt-router"},
        {"labels", json::array({"coding", "cooking", "pii"})},
    };
    auto classifier = lemon::make_classifier(cfg);
    check("make_classifier builds zero_shot classifier",
          classifier->id() == "route" && classifier->type() == "zero_shot" &&
              classifier->labels().size() == 3);

    lemon::testing::FakeClassifierServices fake;
    fake.set_zero_shot_scores("lfm-prompt-router",
                              {{"coding", 0.02}, {"cooking", 0.93}, {"pii", 0.05}});
    // Configured on the other service too: if the classifier reached
    // run_classifier instead, the scores below would be the wrong ones.
    fake.set_classifier_scores("lfm-prompt-router", {{"coding", 1.0}});
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services);
    auto score = classifier->evaluate(lemon::ClassifierContext{ctx.request, ctx.services});
    check("zero_shot classifier calls run_zero_shot_classifier service",
          score.ok && score.score_of("cooking") == 0.93);
    check("zero_shot classifier sends its declared labels",
          fake.last_zero_shot_labels() ==
              std::vector<std::string>{"coding", "cooking", "pii"});
}

static void test_make_zero_shot_classifier_rejections() {
    check("zero_shot rejects missing model",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "zero_shot"},
                                          {"labels", json::array({"a"})}});
          }));
    check("zero_shot rejects missing labels",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "zero_shot"},
                                          {"model", "m"}});
          }));
    check("zero_shot rejects empty labels",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "zero_shot"},
                                          {"model", "m"}, {"labels", json::array()}});
          }));
    check("zero_shot rejects prompt",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "zero_shot"},
                                          {"model", "m"},
                                          {"labels", json::array({"a"})},
                                          {"prompt", "pick one"}});
          }));
    check("zero_shot rejects reference_phrases",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "zero_shot"},
                                          {"model", "m"},
                                          {"labels", json::array({"a"})},
                                          {"reference_phrases", json{{"a", json::array({"p"})}}}});
          }));
}

// An unconfigured run_zero_shot_classifier must surface as Score::ok=false so
// the owning condition applies on_error, not as a throw out of evaluate().
static void test_zero_shot_classifier_without_service_fails_soft() {
    auto classifier = lemon::make_classifier(json{
        {"id", "route"}, {"type", "zero_shot"}, {"model", "m"},
        {"labels", json::array({"a", "b"})}});
    lemon::ClassifierServices services;  // every service left unset
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services);
    auto score = classifier->evaluate(lemon::ClassifierContext{ctx.request, ctx.services});
    check("zero_shot without service reports ok=false", !score.ok);
}

// Scores for any label set other than the declared one must fail the
// classifier, so a protective rule applies on_error instead of silently missing.
static void test_zero_shot_classifier_rejects_mismatched_label_set() {
    auto classifier = lemon::make_classifier(json{
        {"id", "route"}, {"type", "zero_shot"}, {"model", "m"},
        {"labels", json::array({"pii", "chit-chat"})}});
    auto route = make_route_context();

    auto evaluate_with = [&](std::map<std::string, double> returned) {
        lemon::testing::FakeClassifierServices fake;
        fake.set_zero_shot_scores("m", std::move(returned));
        auto services = fake.make();
        EvalContext ctx = make_eval_context(route, services);
        return classifier->evaluate(lemon::ClassifierContext{ctx.request, ctx.services});
    };

    check("zero_shot fails on the backend's own label set",
          !evaluate_with({{"LABEL_0", 0.9}, {"LABEL_1", 0.1}}).ok);
    check("zero_shot fails when a declared label is missing",
          !evaluate_with({{"chit-chat", 1.0}}).ok);
    check("zero_shot fails when an undeclared label is returned",
          !evaluate_with({{"pii", 0.2}, {"chit-chat", 0.7}, {"O", 0.1}}).ok);

    auto exact = evaluate_with({{"pii", 0.8}, {"chit-chat", 0.2}});
    check("zero_shot accepts exactly the declared label set",
          exact.ok && exact.score_of("pii") == 0.8);
}

static lemon::Score evaluate_systemone(const lemon::ClassifierPtr& classifier, json answer,
                                       json* asked = nullptr) {
    lemon::testing::FakeClassifierServices fake;
    fake.set_systemone_answer("decider", std::move(answer));
    auto services = fake.make();
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, services);
    lemon::Score score = classifier->evaluate(lemon::ClassifierContext{ctx.request, ctx.services});
    if (asked) *asked = fake.last_systemone_question();
    return score;
}

static void test_make_systemone_classifier() {
    const json choice_question = {
        {"type", "choice"},
        {"instructions", "Which team should handle this?"},
        {"criteria", {{"billing", nullptr}, {"shipping", "where an order is"}}},
    };
    auto choice = lemon::make_classifier(json{
        {"id", "team"}, {"type", "systemone"}, {"model", "decider"},
        {"question", choice_question}});
    check("make_classifier builds a systemone classifier",
          choice->type() == "systemone" &&
              choice->labels() == std::vector<std::string>{"billing", "shipping"});

    json asked;
    auto score = evaluate_systemone(
        choice,
        {{"type", "choice"}, {"choice", "billing"},
         {"probabilities", {{"billing", 0.9}, {"shipping", 0.1}}}},
        &asked);
    check("systemone sends its declared question", asked == choice_question);
    check("systemone choice reports the option probabilities",
          score.ok && score.score_of("billing") == 0.9 && score.score_of("shipping") == 0.1);

    auto noul = lemon::make_classifier(json{
        {"id", "pii"}, {"type", "systemone"}, {"model", "decider"},
        {"question", {{"type", "noul"},
                      {"instructions", "Does the message contain personal data?"}}}});
    score = evaluate_systemone(noul, {{"type", "noul"}, {"noul", 0.75}});
    check("systemone noul declares true and false",
          noul->labels() == std::vector<std::string>{"true", "false"});
    check("systemone noul reports true and its complement",
          score.ok && score.score_of("true") == 0.75 && score.score_of("false") == 0.25);

    auto level = lemon::make_classifier(json{
        {"id", "urgency"}, {"type", "systemone"}, {"model", "decider"},
        {"question", {{"type", "score"},
                      {"instructions", "How urgent is this?"},
                      {"criteria", json::array({"can wait", "today", "right now"})}}}});
    score = evaluate_systemone(
        level, {{"type", "score"}, {"score", 1.2},
                {"probabilities", {{"0", 0.1}, {"1", 0.6}, {"2", 0.3}}}});
    check("systemone score keys each level probability by its description",
          score.ok && score.score_of("can wait") == 0.1 && score.score_of("today") == 0.6 &&
              score.score_of("right now") == 0.3);
}

// An answer that does not cover the asked options must fail the
// classifier, so a protective rule applies on_error instead of silently missing.
static void test_systemone_classifier_rejects_mismatched_answer() {
    auto choice = lemon::make_classifier(json{
        {"id", "team"}, {"type", "systemone"}, {"model", "decider"},
        {"question", {{"type", "choice"}, {"instructions", "Which team?"},
                      {"criteria", {{"billing", nullptr}, {"shipping", nullptr}}}}}});
    check("systemone fails on another option set",
          !evaluate_systemone(choice, {{"probabilities", {{"LABEL_0", 1.0}}}}).ok);
    check("systemone fails when an option is missing",
          !evaluate_systemone(choice, {{"probabilities", {{"billing", 1.0}}}}).ok);
    check("systemone fails on an empty answer", !evaluate_systemone(choice, json::object()).ok);
    check("systemone fails on a probability outside [0, 1]",
          !evaluate_systemone(choice,
                              {{"probabilities", {{"billing", 1.5}, {"shipping", -0.5}}}}).ok);

    auto level = lemon::make_classifier(json{
        {"id", "urgency"}, {"type", "systemone"}, {"model", "decider"},
        {"question", {{"type", "score"}, {"instructions", "How urgent?"},
                      {"criteria", json::array({"low", "high"})}}}});
    check("systemone score fails on a level index out of range",
          !evaluate_systemone(level, {{"probabilities", {{"0", 0.5}, {"5", 0.5}}}}).ok);

    lemon::ClassifierServices no_services;
    auto route = make_route_context();
    EvalContext ctx = make_eval_context(route, no_services);
    check("systemone without service reports ok=false",
          !choice->evaluate(lemon::ClassifierContext{ctx.request, ctx.services}).ok);
}

static void test_make_systemone_classifier_rejections() {
    auto make = [](json question, json extra = json::object()) {
        json config = {{"id", "x"}, {"type", "systemone"}, {"model", "m"},
                       {"question", std::move(question)}};
        config.update(extra);
        return throws_invalid_arg([config] { lemon::make_classifier(config); });
    };
    const json ok_question = {{"type", "noul"}, {"instructions", "Is it?"}};

    check("systemone rejects a missing question",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "systemone"}, {"model", "m"}});
          }));
    check("systemone rejects a missing model",
          throws_invalid_arg([&] {
              lemon::make_classifier(json{{"id", "x"}, {"type", "systemone"},
                                          {"question", ok_question}});
          }));
    check("systemone rejects labels", make(ok_question, {{"labels", json::array({"a"})}}));
    check("systemone rejects prompt", make(ok_question, {{"prompt", "p"}}));
    check("systemone rejects an unknown question type",
          make({{"type", "rank"}, {"instructions", "?"}}));
    check("systemone rejects a question without instructions", make({{"type", "noul"}}));
    check("systemone rejects an unknown question key",
          make({{"type", "noul"}, {"instructions", "?"}, {"options", json::array()}}));
    check("systemone rejects an empty choice",
          make({{"type", "choice"}, {"instructions", "?"}, {"criteria", json::object()}}));
    check("systemone rejects a one-level score",
          make({{"type", "score"}, {"instructions", "?"}, {"criteria", json::array({"a"})}}));
    check("systemone rejects duplicate score levels",
          make({{"type", "score"}, {"instructions", "?"},
                {"criteria", json::array({"a", "a"})}}));
    check("systemone rejects a non-string score level",
          make({{"type", "score"}, {"instructions", "?"}, {"criteria", json::array({"a", 2})}}));
    check("systemone rejects noul criteria keys other than true and false",
          make({{"type", "noul"}, {"instructions", "?"}, {"criteria", {{"maybe", "?"}}}}));
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
    check("make_classifier accepts semantic_similarity", [] {
        try {
            auto c = lemon::make_classifier(
                json{{"id", "topic"}, {"type", "semantic_similarity"},
                     {"model", "m"},
                     {"reference_phrases", {{"coding", json::array({"write a function"})},
                                            {"math", json::array({"integral"})}}}});
            const auto& labels = c->labels();
            return c && c->id() == "topic" && c->type() == "semantic_similarity" &&
                   labels.size() == 2 &&
                   std::find(labels.begin(), labels.end(), "coding") != labels.end() &&
                   std::find(labels.begin(), labels.end(), "math") != labels.end();
        } catch (...) {
            return false;
        }
    }());
    check("make_classifier accepts semantic_similarity default_label", [] {
        try {
            auto c = lemon::make_classifier(
                json{{"id", "topic"}, {"type", "semantic_similarity"}, {"model", "m"},
                     {"reference_phrases", {{"coding", json::array({"write a function"})}}},
                     {"default_label", "coding"}});
            return c && c->default_label().has_value() && *c->default_label() == "coding";
        } catch (...) {
            return false;
        }
    }());
    check("make_classifier rejects semantic_similarity without reference_phrases",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"}});
          }));
    check("make_classifier rejects semantic_similarity with empty reference_phrases",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", json::object()}});
          }));
    check("make_classifier rejects semantic_similarity concept with empty phrase list",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", {{"coding", json::array()}}}});
          }));
    check("make_classifier rejects semantic_similarity concept with empty phrase string",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", {{"coding", json::array({""})}}}});
          }));
    check("make_classifier rejects semantic_similarity without model",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"reference_phrases", {{"coding", json::array({"x"})}}}});
          }));
    check("make_classifier rejects semantic_similarity with explicit labels",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", {{"coding", json::array({"x"})}}},
                                          {"labels", json::array({"coding"})}});
          }));
    check("make_classifier rejects semantic_similarity default_label not a concept",
          throws_invalid_arg([] {
              lemon::make_classifier(json{{"id", "topic"}, {"type", "semantic_similarity"},
                                          {"model", "m"},
                                          {"reference_phrases", {{"coding", json::array({"x"})}}},
                                          {"default_label", "math"}});
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

static void test_leaf_factory_label_less_primary() {
    // A `classifier` with no declared labels: a condition may omit `label`, and
    // selected_score() falls back to Score::primary().
    auto label_less = lemon::make_classifier(json{
        {"id", "toxicity"},
        {"type", "classifier"},
        {"model", "toxicity-small"},
    });
    auto leaf_factory = lemon::make_leaf_factory({{"toxicity", label_less}});
    auto route = make_route_context();

    // Single-label score: primary() reads the lone entry, so the band applies.
    {
        lemon::testing::FakeClassifierServices fake;
        fake.set_classifier_scores("toxicity-small", {{"toxic", 0.91}});
        auto services = fake.make();
        EvalContext ctx = make_eval_context(route, services);
        auto condition = leaf_factory(json{{"classifier", "toxicity"}, {"min_score", 0.5}});
        check("label-less classifier matches via primary() lone entry",
              condition->evaluate(ctx));
    }

    // Multi-label score: primary() returns 0.0 rather than an arbitrary label,
    // so an unlabeled condition does not silently match the first label.
    {
        lemon::testing::FakeClassifierServices fake;
        fake.set_classifier_scores("toxicity-small", {{"toxic", 0.91}, {"clean", 0.09}});
        auto services = fake.make();
        EvalContext ctx = make_eval_context(route, services);
        auto condition = leaf_factory(json{{"classifier", "toxicity"}, {"min_score", 0.5}});
        check("label-less classifier does not match on a multi-label score",
              !condition->evaluate(ctx));
    }
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
    test_make_zero_shot_classifier();
    test_make_zero_shot_classifier_rejections();
    test_zero_shot_classifier_without_service_fails_soft();
    test_zero_shot_classifier_rejects_mismatched_label_set();
    test_make_systemone_classifier();
    test_systemone_classifier_rejects_mismatched_answer();
    test_make_systemone_classifier_rejections();
    test_make_classifier_rejections();
    test_make_classifiers();
    test_leaf_factory_classifier_refs();
    test_leaf_factory_label_less_primary();
    test_leaf_factory_deterministic_dispatch_and_implicit_all();
    test_leaf_factory_classifier_and_deterministic_implicit_all();

    std::printf("\n%s\n", g_failures == 0 ? "ALL REGISTRY TESTS PASSED"
                                          : "REGISTRY TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
