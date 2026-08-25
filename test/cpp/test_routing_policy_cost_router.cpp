// Unit + end-to-end tests for the `cost` classifier and the `cost_select`
// router-sugar desugaring (see routing_policy_parser.cpp).
//
// Covers: cheapest-candidate selection; excluding candidates with missing,
// invalid, or throwing cost data; falling open to default_model with no
// cost data; memoized ranking; the parser desugaring; and full end-to-end
// routing.
//
// Compile: g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//   test/cpp/test_routing_policy_cost_router.cpp src/cpp/server/routing_policy.cpp \
//   src/cpp/server/routing_policy_parser.cpp -o test_routing_policy_cost_router

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::ClassifierContext;
using lemon::ClassifierPtr;
using lemon::ClassifierServices;
using lemon::CostInfo;
using lemon::CostServices;
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
    route.params.model = "user.Router-Cost";
    route.params.chars = input.size();
    return route;
}

// The l0b_cost_select.json fixture, built inline so the test has no
// working-directory dependency.
static json l0b_collection() {
    return json{
        {"version", "1"},
        {"model_name", "user.Router-Cost"},
        {"recipe", "collection.router"},
        {"components", {"Cheap-GGUF", "Mid-GGUF", "cloud.expensive"}},
        {"routing", {
            {"candidates", {"Cheap-GGUF", "Mid-GGUF", "cloud.expensive"}},
            {"default_model", "Mid-GGUF"},
            {"router", {
                {"type", "cost_select"},
            }},
        }},
    };
}

static RoutePolicy parse_l0b(const json& collection) {
    RoutingPolicyParseOptions options;
    // Identity resolver: component names route to themselves.
    options.resolve_component = [](const std::string& c) {
        return std::optional<std::string>(c);
    };
    return lemon::parse_route_policy_collection(collection, options);
}

// Builds a CostServices whose cost_of looks prices up in `prices` (both
// per-million fields, when the candidate is present) and otherwise returns an
// empty CostInfo (no data). A candidate present in `throwing` throws instead.
static CostServices fake_cost_services(
    std::map<std::string, std::pair<double, double>> prices,
    std::vector<std::string> throwing = {}) {
    CostServices services;
    services.cost_of = [prices, throwing](const std::string& candidate) -> CostInfo {
        for (const auto& t : throwing) {
            if (t == candidate) {
                throw std::runtime_error("cost lookup backend down");
            }
        }
        CostInfo info;
        auto it = prices.find(candidate);
        if (it != prices.end()) {
            info.cost_input_per_million = it->second.first;
            info.cost_output_per_million = it->second.second;
        }
        return info;
    };
    return services;
}

// ---------------------------------------------------------------------------
// Classifier-level behavior
// ---------------------------------------------------------------------------

static ClassifierPtr make_cost(const std::vector<std::string>& candidates) {
    json cfg = {
        {"id", "__router"},
        {"type", "cost"},
        {"labels", candidates},
    };
    return lemon::make_classifier(cfg);
}

static void test_cheapest_wins_all_priced() {
    auto cost = make_cost({"Cheap-GGUF", "Mid-GGUF", "cloud.expensive"});
    CostServices services = fake_cost_services({
        {"Cheap-GGUF", {0.10, 0.20}},   // sum 0.30
        {"Mid-GGUF", {1.0, 2.0}},       // sum 3.0
        {"cloud.expensive", {5.0, 15.0}},  // sum 20.0
    });
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: cheapest candidate wins with all priced",
          s.ok && s.labels.size() == 1 && s.score_of("Cheap-GGUF") == 1.0);
}

static void test_tie_break_first_in_order() {
    auto cost = make_cost({"Mid-GGUF", "Cheap-GGUF"});  // Cheap-GGUF listed second
    CostServices services = fake_cost_services({
        {"Mid-GGUF", {1.0, 2.0}},    // sum 3.0
        {"Cheap-GGUF", {1.5, 1.5}},  // sum 3.0 -- exact tie
    });
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: exact tie resolves to the first-in-order candidate",
          s.ok && s.score_of("Mid-GGUF") == 1.0);
}

static void test_fallback_no_data_anywhere_yields_no_label() {
    auto cost = make_cost({"Cheap-GGUF", "Mid-GGUF", "cloud.expensive"});
    CostServices services = fake_cost_services({});  // no candidate has data
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: no data anywhere yields an empty Score (fails open, no winning label)",
          s.ok && s.labels.empty());
}

static void test_mixed_priced_and_unpriced_candidates() {
    // The unpriced candidate is listed first; it must not win by default.
    auto cost = make_cost({"cloud.expensive", "Mid-GGUF"});
    CostServices services = fake_cost_services({
        {"Mid-GGUF", {1.0, 2.0}},
        // "cloud.expensive" deliberately absent from `prices` -> no data.
    });
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: a correctly-priced candidate beats an unpriced one listed first",
          s.ok && s.score_of("Mid-GGUF") == 1.0);
}

static void test_partial_price_data_treated_as_no_data() {
    // "Mid-GGUF" only resolves cost_input_per_million; per compute_cost_score's
    // contract, a candidate must resolve BOTH fields to get a score.
    auto cost = make_cost({"Mid-GGUF", "Cheap-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 0.01;  // output price deliberately unset
        } else if (candidate == "Cheap-GGUF") {
            info.cost_input_per_million = 10.0;
            info.cost_output_per_million = 10.0;  // sum 20.0, the only real score
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: a candidate with only one of the two per-million fields is unranked",
          s.ok && s.score_of("Cheap-GGUF") == 1.0);
}

static void test_throwing_candidate_excluded_not_a_failure() {
    auto cost = make_cost({"Mid-GGUF", "Cheap-GGUF"});
    CostServices services = fake_cost_services(
        {
            {"Mid-GGUF", {1.0, 2.0}},
            {"Cheap-GGUF", {0.1, 0.1}},
        },
        /*throwing=*/{"Cheap-GGUF"});
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: a throwing candidate is excluded from ranking, not a classifier failure",
          s.ok && s.score_of("Mid-GGUF") == 1.0);
}

static void test_negative_or_nonfinite_price_excluded_from_ranking() {
    auto cost = make_cost({"Bad-Negative", "Bad-NaN", "Bad-Inf", "Mid-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Bad-Negative") {
            info.cost_input_per_million = -1.0;   // invalid: negative
            info.cost_output_per_million = 0.5;
        } else if (candidate == "Bad-NaN") {
            info.cost_input_per_million = std::nan("");  // invalid: NaN
            info.cost_output_per_million = 0.5;
        } else if (candidate == "Bad-Inf") {
            info.cost_input_per_million = std::numeric_limits<double>::infinity();  // invalid: inf
            info.cost_output_per_million = 0.5;
        } else if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 1.0;
            info.cost_output_per_million = 2.0;  // sum 3.0, the only valid price
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: negative/NaN/inf prices are excluded from ranking, not treated as cheapest",
          s.ok && s.score_of("Mid-GGUF") == 1.0);
}

static void test_free_tier_wins_over_priced_candidates() {
    auto cost = make_cost({"cloud.cheap", "Local-Free-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "cloud.cheap") {
            info.cost_input_per_million = 0.01;
            info.cost_output_per_million = 0.01;  // sum 0.02 -- tiny, but still priced
        } else if (candidate == "Local-Free-GGUF") {
            info.cost_tier = "free";  // no per-million fields at all
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: cost_tier=free beats a priced candidate, even a cheap one",
          s.ok && s.score_of("Local-Free-GGUF") == 1.0);
}

static void test_free_tier_short_circuits_contradictory_per_million_fields() {
    auto cost = make_cost({"Weird-Free-But-Priced"});
    CostServices services;
    services.cost_of = [](const std::string&) -> CostInfo {
        CostInfo info;
        info.cost_tier = "free";
        info.cost_input_per_million = 999.0;   // contradictory; must be ignored
        info.cost_output_per_million = 999.0;
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: cost_tier=free short-circuits even a contradictory high per-million price",
          s.ok && s.score_of("Weird-Free-But-Priced") == 1.0);
}

static void test_free_tier_rationale_says_free_not_per_million_sum() {
    auto cost = make_cost({"Local-Free-GGUF", "Mid-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Local-Free-GGUF") {
            info.cost_tier = "free";
        } else if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 1.0;
            info.cost_output_per_million = 2.0;
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: free-tier winner's rationale says cost_tier=free, not a per-M sum",
          s.ok && s.score_of("Local-Free-GGUF") == 1.0 &&
          s.rationale.find("cost_tier=free") != std::string::npos &&
          s.rationale.find("per-M sum") == std::string::npos);
}

static void test_sum_overflow_excluded_from_ranking() {
    auto cost = make_cost({"Bad-Overflow", "Mid-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Bad-Overflow") {
            info.cost_input_per_million = std::numeric_limits<double>::max();
            info.cost_output_per_million = std::numeric_limits<double>::max();  // sum -> +inf
        } else if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 1.0;
            info.cost_output_per_million = 2.0;  // sum 3.0, the only valid score
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: two finite prices whose sum overflows to inf are excluded from ranking",
          s.ok && s.score_of("Mid-GGUF") == 1.0);
}

static void test_price_below_sanity_floor_excluded_but_exact_zero_is_not() {
    auto cost = make_cost({"Spoofed-Tiny", "Legit-Zero", "Mid-GGUF"});
    CostServices services;
    services.cost_of = [](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Spoofed-Tiny") {
            info.cost_input_per_million = 1e-30;
            info.cost_output_per_million = 1e-30;  // implausibly tiny (spoofed/underflowed)
        } else if (candidate == "Legit-Zero") {
            info.cost_input_per_million = 0.0;
            info.cost_output_per_million = 0.0;  // an honestly-declared free price via numbers
        } else if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 1.0;
            info.cost_output_per_million = 2.0;
        }
        return info;
    };
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: an implausibly tiny positive price is excluded, but exact 0.0 still wins as free",
          s.ok && s.score_of("Legit-Zero") == 1.0);
}

static void test_ranking_reflects_a_price_change_on_the_next_evaluate_call() {
    // The classifier itself must not cache across evaluate() calls: it is
    // shared (via RoutePolicy::classifiers' shared_ptr) by every request's
    // engine for as long as the collection's policy is loaded, so a
    // classifier-level cache would freeze the winner at whatever it was on
    // first use. Any request-scale caching belongs in CostServices::cost_of,
    // which is free to memoize as long as it invalidates on real changes.
    auto cost = make_cost({"Cheap-GGUF", "Mid-GGUF"});
    auto price = std::make_shared<double>(0.1);
    CostServices services;
    services.cost_of = [price](const std::string& candidate) -> CostInfo {
        CostInfo info;
        if (candidate == "Cheap-GGUF") {
            info.cost_input_per_million = *price;
            info.cost_output_per_million = *price;
        } else if (candidate == "Mid-GGUF") {
            info.cost_input_per_million = 1.0;
            info.cost_output_per_million = 2.0;
        }
        return info;
    };
    ClassifierContext ctx{make_route("hi"), ClassifierServices{}, services};
    Score before = cost->evaluate(ctx);
    *price = 10.0;  // Cheap-GGUF just got more expensive than Mid-GGUF.
    Score after = cost->evaluate(ctx);
    check("cost: a price change is reflected on the very next evaluate() call",
          before.score_of("Cheap-GGUF") == 1.0 && after.score_of("Mid-GGUF") == 1.0);
}

static void test_rationale_uses_locale_independent_formatting() {
    auto cost = make_cost({"Cheap-GGUF", "Mid-GGUF"});
    CostServices services = fake_cost_services({
        {"Cheap-GGUF", {0.10, 0.20}},  // sum 0.30
        {"Mid-GGUF", {1.0, 2.0}},
    });
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, services});
    check("cost: rationale renders the cost figure without to_string(double)'s 6-decimal padding",
          s.ok && s.rationale.find("0.3000") != std::string::npos &&
          s.rationale.find("0.300000") == std::string::npos);
}

static void test_unset_cost_services_is_classifier_failure() {
    auto cost = make_cost({"Cheap-GGUF", "Mid-GGUF"});
    Score s = cost->evaluate(ClassifierContext{make_route("hi"), ClassifierServices{}, CostServices{}});
    check("cost: a wholly unset CostServices yields Score::ok=false", !s.ok);
}

static void test_make_classifier_rejects_cost_without_labels() {
    bool threw = false;
    try {
        lemon::make_classifier(json{{"id", "__router"}, {"type", "cost"}, {"labels", json::array()}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("cost: make_classifier rejects an empty label list", threw);
}

// ---------------------------------------------------------------------------
// Parser desugaring
// ---------------------------------------------------------------------------

static void test_desugar_shape() {
    RoutePolicy policy = parse_l0b(l0b_collection());

    check("desugar(cost_select): candidates preserved",
          policy.candidates.size() == 3 &&
              policy.candidates[0] == "Cheap-GGUF" &&
              policy.candidates[1] == "Mid-GGUF" &&
              policy.candidates[2] == "cloud.expensive");
    check("desugar(cost_select): default_model preserved", policy.default_model == "Mid-GGUF");
    check("desugar(cost_select): exactly one synthesized cost classifier",
          policy.classifiers.size() == 1 && policy.classifiers.count("__router") == 1 &&
              policy.classifiers.at("__router")->type() == "cost");
    check("desugar(cost_select): one identity rule per candidate",
          policy.rules.size() == 3 &&
              policy.rules[0].route_to == "Cheap-GGUF" &&
              policy.rules[1].route_to == "Mid-GGUF" &&
              policy.rules[2].route_to == "cloud.expensive");
}

static void test_desugar_normalized_routing_out_param() {
    RoutingPolicyParseOptions options;
    options.resolve_component = [](const std::string& c) {
        return std::optional<std::string>(c);
    };
    json normalized;
    lemon::parse_route_policy_collection(l0b_collection(), options, &normalized);

    check("desugar(cost_select): normalized routing drops router", !normalized.contains("router"));
    check("desugar(cost_select): normalized routing carries the synthesized cost classifier",
          normalized.contains("classifiers") && normalized["classifiers"].size() == 1 &&
              normalized["classifiers"][0]["id"] == "__router" &&
              normalized["classifiers"][0]["type"] == "cost" &&
              !normalized["classifiers"][0].contains("model") &&
              !normalized["classifiers"][0].contains("prompt"));
    check("desugar(cost_select): normalized routing carries one identity rule per candidate",
          normalized.contains("rules") && normalized["rules"].size() == 3 &&
              normalized["rules"][0]["id"] == "__route_0" &&
              normalized["rules"][0]["route_to"] == "Cheap-GGUF");
}

static void test_desugar_rejects_router_plus_rules() {
    json bad = l0b_collection();
    bad["routing"]["rules"] = json::array({json{
        {"id", "r0"}, {"match", {{"keywords_any", {"x"}}}}, {"route_to", "Cheap-GGUF"}}});
    bool threw = false;
    try { parse_l0b(bad); } catch (const std::invalid_argument&) { threw = true; }
    check("desugar(cost_select): router + explicit rules is rejected", threw);
}

static void test_desugar_rejects_model_or_prompt() {
    json bad = l0b_collection();
    bad["routing"]["router"]["model"] = "some-model";
    bool threw = false;
    try { parse_l0b(bad); } catch (const std::invalid_argument&) { threw = true; }
    check("desugar(cost_select): a 'model' key on the sugar is rejected", threw);

    json bad2 = l0b_collection();
    bad2["routing"]["router"]["prompt"] = "pick one";
    bool threw2 = false;
    try { parse_l0b(bad2); } catch (const std::invalid_argument&) { threw2 = true; }
    check("desugar(cost_select): a 'prompt' key on the sugar is rejected", threw2);
}

static void test_desugar_rejects_unknown_router_type() {
    json bad = l0b_collection();
    bad["routing"]["router"]["type"] = "something_else";
    bool threw = false;
    try { parse_l0b(bad); } catch (const std::invalid_argument&) { threw = true; }
    check("desugar: an unknown routing.router.type is rejected", threw);
}

// /v1/routing/validate parses routing.candidates with an identity resolver —
// no registered-model count bounds it there — so the cap is enforced in the
// parser itself, not only as documentation in the schema's maxItems.
static void test_candidates_cap_enforced() {
    json collection = l0b_collection();
    std::vector<std::string> many;
    for (int i = 0; i < 65; ++i) {
        many.push_back("Candidate-" + std::to_string(i));
    }
    collection["components"] = many;
    collection["routing"]["candidates"] = many;
    collection["routing"]["default_model"] = many[0];
    bool threw = false;
    try { parse_l0b(collection); } catch (const std::invalid_argument&) { threw = true; }
    check("cost_select: routing.candidates over the cap (65) is rejected", threw);
}

// ---------------------------------------------------------------------------
// End-to-end engine routing (the acceptance path)
// ---------------------------------------------------------------------------

static void test_e2e_routes_to_cheapest_and_reports_its_own_cost() {
    RoutePolicy policy = parse_l0b(l0b_collection());
    CostServices services = fake_cost_services({
        {"Cheap-GGUF", {0.10, 0.20}},
        {"Mid-GGUF", {1.0, 2.0}},
        {"cloud.expensive", {5.0, 15.0}},
    });
    RoutingPolicyEngine engine(std::move(policy), ClassifierServices{}, services);
    Decision d = engine.route(make_route("anything"), /*want_trace=*/true);

    check("e2e(cost_select): routes to the cheapest candidate",
          d.route_to == "Cheap-GGUF" && !d.default_used);

    // attach_estimated_cost runs after route_to is resolved, using the SAME
    // CostServices — prove it composes correctly with the new pre-selection
    // classifier and still reports the winner's own cost.
    check("e2e(cost_select): estimated_cost reflects the winning candidate's own price",
          d.outputs.contains("estimated_cost") &&
          d.outputs["estimated_cost"].value("cost_input_per_million", -1.0) == 0.10 &&
          d.outputs["estimated_cost"].value("cost_output_per_million", -1.0) == 0.20);

    bool saw_router_trace = false;
    for (const auto& e : d.trace) {
        if (e.condition == "classifier:__router" && e.result && e.label == "Cheap-GGUF") {
            saw_router_trace = true;
        }
    }
    check("e2e(cost_select): trace identifies the winning candidate", saw_router_trace);
}

static void test_e2e_no_data_falls_open_to_default_model() {
    // default_model is "Mid-GGUF". With no candidate priced, the cost
    // classifier reports no winning label, so no identity rule matches and
    // the engine falls through to default_model.
    RoutePolicy policy = parse_l0b(l0b_collection());
    CostServices services = fake_cost_services({});  // no candidate has data
    RoutingPolicyEngine engine(std::move(policy), ClassifierServices{}, services);
    Decision d = engine.route(make_route("anything"), /*want_trace=*/false);

    check("e2e(cost_select): no cost data anywhere falls open to default_model",
          d.route_to == "Mid-GGUF" && d.default_used);
}

int main() {
    test_cheapest_wins_all_priced();
    test_tie_break_first_in_order();
    test_fallback_no_data_anywhere_yields_no_label();
    test_mixed_priced_and_unpriced_candidates();
    test_partial_price_data_treated_as_no_data();
    test_negative_or_nonfinite_price_excluded_from_ranking();
    test_free_tier_wins_over_priced_candidates();
    test_free_tier_short_circuits_contradictory_per_million_fields();
    test_free_tier_rationale_says_free_not_per_million_sum();
    test_sum_overflow_excluded_from_ranking();
    test_price_below_sanity_floor_excluded_but_exact_zero_is_not();
    test_ranking_reflects_a_price_change_on_the_next_evaluate_call();
    test_rationale_uses_locale_independent_formatting();
    test_throwing_candidate_excluded_not_a_failure();
    test_unset_cost_services_is_classifier_failure();
    test_make_classifier_rejects_cost_without_labels();
    test_desugar_shape();
    test_desugar_normalized_routing_out_param();
    test_desugar_rejects_router_plus_rules();
    test_desugar_rejects_model_or_prompt();
    test_desugar_rejects_unknown_router_type();
    test_candidates_cap_enforced();
    test_e2e_routes_to_cheapest_and_reports_its_own_cost();
    test_e2e_no_data_falls_open_to_default_model();

    if (g_failures == 0) {
        std::printf("All cost router tests passed.\n");
    } else {
        std::printf("%d cost router test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
