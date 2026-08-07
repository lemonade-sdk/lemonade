// Unit tests for the Lemonade Router semantic_similarity classifier (#2381).
//
// Covers max-cosine computation against reference phrases, reference-phrase
// embedding caching (embed called once per phrase), inclusive classifier-band
// boundaries (incl. the default min_score of 0.5), and on_error handling when
// the embedder fails. All backend access is faked via FakeClassifierServices.

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using lemon::ClassifierContext;
using lemon::ClassifierPtr;
using lemon::ClassifierServices;
using lemon::Condition;
using lemon::ConditionPtr;
using lemon::EvalContext;
using lemon::RouteContext;
using lemon::Score;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

static RouteContext make_route(const std::string& input) {
    RouteContext route;
    route.input = input;
    route.params.model = "user.Router";
    route.params.chars = input.size();
    return route;
}

static ClassifierPtr make_sim(
    const std::string& model,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& concepts,
    const char* on_error = "match_false",
    std::optional<std::string> default_label = std::nullopt) {
    json cfg = {
        {"id", "topic"},
        {"type", "semantic_similarity"},
        {"model", model},
        {"on_error", on_error},
        {"reference_phrases", json::object()},
    };
    for (const auto& concept : concepts) {
        cfg["reference_phrases"][concept.first] = json::array();
        for (const auto& phrase : concept.second) {
            cfg["reference_phrases"][concept.first].push_back(phrase);
        }
    }
    if (default_label) cfg["default_label"] = *default_label;
    return lemon::make_classifier(cfg);
}

static void test_per_concept_scores() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "code phrase", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "math phrase", {0.0f, 1.0f, 0.0f});
    // Input aligns with the coding concept -> cosine 1.0 there, 0.0 with math.
    fake.set_embedding(model, "find the bug", {1.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {{"coding", {"code phrase"}}, {"math", {"math phrase"}}});
    RouteContext route = make_route("find the bug");
    Score score = sim->evaluate(ClassifierContext{route, svc});

    check("semantic_similarity reports one score per concept",
          score.ok && score.labels.size() == 2 &&
              score.labels.count("coding") == 1 && score.labels.count("math") == 1);
    check("semantic_similarity scores the matching concept high",
          near(score.score_of("coding"), 1.0));
    check("semantic_similarity scores the non-matching concept low",
          near(score.score_of("math"), 0.0));
}

static void test_max_within_concept() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    // Concept "coding" has two phrases; the input matches the second exactly.
    fake.set_embedding(model, "a", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "b", {0.0f, 1.0f, 0.0f});
    fake.set_embedding(model, "q", {0.0f, 1.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {{"coding", {"a", "b"}}});
    RouteContext route = make_route("q");
    Score score = sim->evaluate(ClassifierContext{route, svc});
    check("concept score is the max cosine across its phrases",
          score.ok && near(score.score_of("coding"), 1.0));
}

static void test_labels_are_concept_names() {
    lemon::testing::FakeClassifierServices fake;
    auto sim = make_sim("embed-m", {{"coding", {"a"}}, {"math", {"b"}}});
    const auto& labels = sim->labels();
    bool has_both = labels.size() == 2 &&
                    std::find(labels.begin(), labels.end(), "coding") != labels.end() &&
                    std::find(labels.begin(), labels.end(), "math") != labels.end();
    check("labels() exposes the concept names", has_both);
}

static void test_reference_caching() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "a", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "b", {0.0f, 1.0f, 0.0f});
    fake.set_embedding(model, "c", {0.0f, 0.0f, 1.0f});
    fake.set_embedding(model, "q", {1.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {{"coding", {"a", "b"}}, {"math", {"c"}}});
    RouteContext route = make_route("q");

    sim->evaluate(ClassifierContext{route, svc});
    sim->evaluate(ClassifierContext{route, svc});
    sim->evaluate(ClassifierContext{route, svc});

    check("each reference phrase embedded exactly once across concepts",
          fake.embed_calls("a") == 1 && fake.embed_calls("b") == 1 &&
              fake.embed_calls("c") == 1);
    check("input embedded once per evaluation", fake.embed_calls("q") == 3);
}

// Build a band condition over a similarity classifier (selecting `label`) and
// evaluate it.
static bool eval_band(const ClassifierPtr& sim, const ClassifierServices& svc,
                      const RouteContext& route, std::optional<std::string> label,
                      std::optional<double> min_score, std::optional<double> max_score) {
    ConditionPtr cond = lemon::make_classifier_band_condition(
        sim, std::move(label), min_score, max_score);
    EvalContext ctx{route, svc};
    return cond->evaluate(ctx);
}

static void test_band_boundaries() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    // Vectors chosen so the cosine is exactly 0.5 in IEEE floating point:
    // dot = 1, |input| = 1, |ref| = sqrt(4) = 2 -> 1 / 2 = 0.5.
    fake.set_embedding(model, "phrase", {1.0f, 1.0f, 1.0f, 1.0f});
    fake.set_embedding(model, "half", {1.0f, 0.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {{"coding", {"phrase"}}});
    RouteContext route = make_route("half");

    check("default band min_score 0.5 includes a score of exactly 0.5",
          eval_band(sim, svc, route, "coding", std::nullopt, std::nullopt));
    check("min_score boundary is inclusive (0.5 >= 0.5)",
          eval_band(sim, svc, route, "coding", 0.5, std::nullopt));
    check("max_score boundary is inclusive (0.5 <= 0.5)",
          eval_band(sim, svc, route, "coding", std::nullopt, 0.5));
    check("score below min_score does not match",
          !eval_band(sim, svc, route, "coding", 0.51, std::nullopt));
    check("score above max_score does not match",
          !eval_band(sim, svc, route, "coding", std::nullopt, 0.49));

    // A condition may omit `label` when the classifier declares a default_label.
    auto sim_default = make_sim(model, {{"coding", {"phrase"}}}, "match_false", "coding");
    check("default_label selects the concept when label is omitted",
          eval_band(sim_default, svc, route, std::nullopt, std::nullopt, std::nullopt));
}

static void test_on_error() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "phrase", {1.0f, 0.0f, 0.0f});
    // An empty input embedding forces a cosine failure -> Score::ok=false.
    fake.set_embedding(model, "boom", std::vector<float>{});
    ClassifierServices svc = fake.make();

    RouteContext route = make_route("boom");

    auto sim_false = make_sim(model, {{"coding", {"phrase"}}}, "match_false");
    Score s = sim_false->evaluate(ClassifierContext{route, svc});
    check("embed failure yields Score::ok=false", !s.ok);
    check("on_error match_false fails open (no match)",
          !eval_band(sim_false, svc, route, "coding", std::nullopt, std::nullopt));

    auto sim_true = make_sim(model, {{"coding", {"phrase"}}}, "match_true");
    check("on_error match_true fails closed (matches)",
          eval_band(sim_true, svc, route, "coding", std::nullopt, std::nullopt));
}

int main() {
    test_per_concept_scores();
    test_max_within_concept();
    test_labels_are_concept_names();
    test_reference_caching();
    test_band_boundaries();
    test_on_error();

    if (g_failures == 0) {
        std::printf("All semantic_similarity classifier tests passed.\n");
    } else {
        std::printf("%d semantic_similarity classifier test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
