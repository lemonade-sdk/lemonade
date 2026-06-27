// Unit tests for the Lemonade Router semantic_similarity classifier (#2381).
//
// Covers max-cosine computation against reference phrases, reference-phrase
// embedding caching (embed called once per phrase), inclusive classifier-band
// boundaries (incl. the default min_score of 0.5), and on_error handling when
// the embedder fails. All backend access is faked via FakeClassifierServices.

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
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

static ClassifierPtr make_sim(const std::string& model,
                              std::vector<std::string> phrases,
                              const char* on_error = "match_false") {
    json cfg = {
        {"id", "is_coding"},
        {"type", "semantic_similarity"},
        {"model", model},
        {"on_error", on_error},
        {"reference_phrases", json::array()},
    };
    for (const auto& phrase : phrases) cfg["reference_phrases"].push_back(phrase);
    return lemon::make_classifier(cfg);
}

static void test_max_cosine() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "alpha", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "beta", {0.0f, 1.0f, 0.0f});
    // Input aligns exactly with "alpha" -> cosine 1.0 there, 0.0 with "beta".
    fake.set_embedding(model, "find the bug", {1.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {"alpha", "beta"});
    RouteContext route = make_route("find the bug");
    Score score = sim->evaluate(ClassifierContext{route, svc});

    check("semantic_similarity reports a single empty-key score",
          score.ok && score.labels.size() == 1 && score.labels.count("") == 1);
    check("semantic_similarity returns max cosine across references",
          near(score.primary(), 1.0));
}

static void test_max_cosine_partial() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "alpha", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "beta", {0.0f, 1.0f, 0.0f});
    // 45 degrees from both axes -> cosine 1/sqrt(2) with each; max is that.
    fake.set_embedding(model, "mixed", {1.0f, 1.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {"alpha", "beta"});
    RouteContext route = make_route("mixed");
    Score score = sim->evaluate(ClassifierContext{route, svc});
    check("semantic_similarity computes correct intermediate cosine",
          score.ok && near(score.primary(), 1.0 / std::sqrt(2.0)));
}

static void test_reference_caching() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "alpha", {1.0f, 0.0f, 0.0f});
    fake.set_embedding(model, "beta", {0.0f, 1.0f, 0.0f});
    fake.set_embedding(model, "q", {1.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {"alpha", "beta"});
    RouteContext route = make_route("q");

    sim->evaluate(ClassifierContext{route, svc});
    sim->evaluate(ClassifierContext{route, svc});
    sim->evaluate(ClassifierContext{route, svc});

    check("reference phrase 'alpha' embedded exactly once", fake.embed_calls("alpha") == 1);
    check("reference phrase 'beta' embedded exactly once", fake.embed_calls("beta") == 1);
    check("input embedded once per evaluation", fake.embed_calls("q") == 3);
}

// Build a band condition over a similarity classifier and evaluate it.
static bool eval_band(const ClassifierPtr& sim, const ClassifierServices& svc,
                      const RouteContext& route, std::optional<double> min_score,
                      std::optional<double> max_score) {
    ConditionPtr cond = lemon::make_classifier_band_condition(
        sim, std::nullopt, min_score, max_score);
    EvalContext ctx{route, svc};
    return cond->evaluate(ctx);
}

static void test_band_boundaries() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    // Vectors chosen so the cosine is exactly 0.5 in IEEE floating point:
    // dot = 1, |input| = 1, |ref| = sqrt(4) = 2 -> 1 / 2 = 0.5.
    fake.set_embedding(model, "alpha", {1.0f, 1.0f, 1.0f, 1.0f});
    fake.set_embedding(model, "half", {1.0f, 0.0f, 0.0f, 0.0f});
    ClassifierServices svc = fake.make();

    auto sim = make_sim(model, {"alpha"});
    RouteContext route = make_route("half");

    check("default band min_score 0.5 includes a score of exactly 0.5",
          eval_band(sim, svc, route, std::nullopt, std::nullopt));
    check("min_score boundary is inclusive (0.5 >= 0.5)",
          eval_band(sim, svc, route, 0.5, std::nullopt));
    check("max_score boundary is inclusive (0.5 <= 0.5)",
          eval_band(sim, svc, route, std::nullopt, 0.5));
    check("score below min_score does not match",
          !eval_band(sim, svc, route, 0.51, std::nullopt));
    check("score above max_score does not match",
          !eval_band(sim, svc, route, std::nullopt, 0.49));
}

static void test_on_error() {
    lemon::testing::FakeClassifierServices fake;
    const std::string model = "embed-m";
    fake.set_embedding(model, "alpha", {1.0f, 0.0f, 0.0f});
    // An empty input embedding forces a cosine failure -> Score::ok=false.
    fake.set_embedding(model, "boom", std::vector<float>{});
    ClassifierServices svc = fake.make();

    RouteContext route = make_route("boom");

    auto sim_false = make_sim(model, {"alpha"}, "match_false");
    Score s = sim_false->evaluate(ClassifierContext{route, svc});
    check("embed failure yields Score::ok=false", !s.ok);
    check("on_error match_false fails open (no match)",
          !eval_band(sim_false, svc, route, std::nullopt, std::nullopt));

    auto sim_true = make_sim(model, {"alpha"}, "match_true");
    check("on_error match_true fails closed (matches)",
          eval_band(sim_true, svc, route, std::nullopt, std::nullopt));
}

int main() {
    test_max_cosine();
    test_max_cosine_partial();
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
