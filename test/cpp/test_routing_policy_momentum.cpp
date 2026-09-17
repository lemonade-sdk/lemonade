// Unit tests for the opt-in routing.momentum filter (route-flapping fix).
//
// Covers three layers: the pure compute_effective_chars() filter arithmetic
// (rise/fall/degenerate coefficients, ties, empty input), CharsCondition's
// evaluate() reading EvalContext::effective_chars instead of
// RouteContext::Params::chars when momentum is active (including the trace
// rationale format), and RoutingPolicyEngine::route() folding
// RoutePolicy::Momentum + RouteContext::user_turn_chars into that per-request
// value before rule evaluation. Absent/disabled momentum must reproduce
// today's byte-count behavior exactly.
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       test/cpp/test_routing_policy_momentum.cpp src/cpp/server/routing_policy.cpp \
//       -o test_routing_policy_momentum

#include "lemon/routing_policy.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using lemon::ClassifierServices;
using lemon::compute_effective_chars;
using lemon::ConditionPtr;
using lemon::Decision;
using lemon::EvalContext;
using lemon::MatchExpr;
using lemon::NamedLeafFactories;
using lemon::RouteContext;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::Rule;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

namespace {

bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// Mirrors test_routing_policy_deterministic.cpp's local helper: build the
// {"min_chars"|"max_chars": threshold} leaf via the real factory registry.
ConditionPtr build_chars_leaf(const std::string& op, long long threshold) {
    NamedLeafFactories factories = lemon::make_deterministic_leaf_factories();
    json leaf = json::object();
    leaf[op] = threshold;
    return factories.at(op)(leaf);
}

MatchExpr deterministic_leaf(json leaf) {
    MatchExpr expr;
    expr.op = MatchExpr::Op::Leaf;
    expr.leaf = std::move(leaf);
    return expr;
}

Rule make_rule(const std::string& id, MatchExpr match, const std::string& route_to) {
    Rule rule;
    rule.id = id;
    rule.match = std::move(match);
    rule.route_to = route_to;
    return rule;
}

void test_compute_effective_chars_single_element() {
    check("single element passthrough (any coefficients)",
          compute_effective_chars({11}, 1.0, 0.3) == 11.0);
    check("single element passthrough (dyadic coefficients)",
          compute_effective_chars({2000}, 0.5, 0.25) == 2000.0);
}

void test_compute_effective_chars_dyadic_exact() {
    // 100 -> 1000 -> 50, attack=0.5, release=0.25 (issue's suggested example):
    // m1=100; m2=100+0.5*900=550; m3=550+0.25*(50-550)=550-125=425.0 — every
    // intermediate value is an exactly-representable double, so this must hold
    // bit-for-bit, not just within an epsilon.
    const double m = compute_effective_chars({100, 1000, 50}, 0.5, 0.25);
    check("dyadic rise-then-fall is IEEE-exact", m == 425.0);
}

void test_compute_effective_chars_tie_uses_attack() {
    // c_i == m_prev counts as "rising" (>=), so the tie uses `attack`, not
    // `release`. {50, 2000, 2000}: m1=50, m2=50+1.0*1950=2000,
    // m3 ties at c==m_prev=2000 => attack path => m3=2000+1.0*0=2000.
    const double m = compute_effective_chars({50, 2000, 2000}, 1.0, 0.1);
    check("c_i == m_prev is treated as rising (uses attack)", m == 2000.0);
}

void test_compute_effective_chars_degenerate_attack_release_one() {
    // attack=release=1.0 must degenerate to the raw last-turn length exactly,
    // for both rising and falling sequences, since it's the compatibility
    // guarantee for "momentum enabled with trivial coefficients == no-op".
    struct Case { std::vector<std::size_t> seq; };
    const Case cases[] = {
        {{5}}, {{5, 5000}}, {{5000, 5}}, {{100, 1000, 50, 2, 9999}},
    };
    for (const auto& c : cases) {
        const double m = compute_effective_chars(c.seq, 1.0, 1.0);
        check("attack=release=1.0 degenerates to raw last turn (bit-exact)",
              m == static_cast<double>(c.seq.back()));
    }
}

void test_compute_effective_chars_default_coefficients() {
    // Defaults from the issue: attack=1.0 (instant escalation), release=0.3
    // (slow decay). 0.3 isn't binary-exact, so this needs an epsilon check.
    // m1=2000; m2=2000+0.3*(11-2000)=2000-596.7=1403.3
    const double m = compute_effective_chars({2000, 11}, 1.0, 0.3);
    check("default release=0.3 decays slowly, not instantly", nearly_equal(m, 1403.3));
    check("default release=0.3 keeps effective length well above the raw follow-up",
          m > 1000.0);
}

void test_compute_effective_chars_empty_is_defensive_zero() {
    check("empty series returns 0.0 (defensive; build_route_context never emits this)",
          compute_effective_chars({}, 1.0, 0.3) == 0.0);
}

void test_chars_condition_momentum_inactive_matches_today() {
    // ctx.effective_chars unset (nullopt) => byte-identical to pre-momentum
    // behavior: compares against params.chars, no rationale attached.
    ConditionPtr cond = build_chars_leaf("min_chars", 1000);
    ClassifierServices services;
    RouteContext req;
    req.input = std::string(11, 'x');
    req.params.chars = req.input.size();
    EvalContext ctx{req, services};
    ctx.want_trace = true;
    const bool result = cond->evaluate(ctx);
    check("momentum inactive: raw 11 bytes misses min_chars:1000", !result);
    check("momentum inactive: trace has no rationale",
          ctx.trace.size() == 1 && ctx.trace[0].rationale.empty());
}

void test_chars_condition_momentum_active_overrides_raw() {
    // effective_chars=1490.5 with raw params.chars=11 (the issue's own worked
    // example): min_chars:1000 must match on the momentum value even though
    // the raw last-turn length alone would miss it, and the trace rationale
    // must read exactly "effective_chars=1490.5 raw_chars=11".
    ConditionPtr cond = build_chars_leaf("min_chars", 1000);
    ClassifierServices services;
    RouteContext req;
    req.input = std::string(11, 'x');
    req.params.chars = req.input.size();
    EvalContext ctx{req, services};
    ctx.want_trace = true;
    ctx.effective_chars = 1490.5;
    const bool result = cond->evaluate(ctx);
    check("momentum active: effective 1490.5 satisfies min_chars:1000 despite raw 11", result);
    check("momentum active: trace condition name unchanged",
          ctx.trace.size() == 1 && ctx.trace[0].condition == "min_chars");
    check("momentum active: rationale format matches issue's example",
          ctx.trace.size() == 1 &&
              ctx.trace[0].rationale == "effective_chars=1490.5 raw_chars=11");
}

void test_chars_condition_momentum_active_max_chars() {
    ConditionPtr cond = build_chars_leaf("max_chars", 100);
    ClassifierServices services;
    RouteContext req;
    req.input = std::string(500, 'x');
    req.params.chars = req.input.size();
    EvalContext ctx{req, services};
    ctx.want_trace = true;
    ctx.effective_chars = 50.0;  // decayed well below the raw 500-byte turn
    const bool result = cond->evaluate(ctx);
    check("momentum active: max_chars uses effective length, not raw", result);
    // nlohmann's dump() appends ".0" to an exact-integer-valued double so the
    // JSON serialization still reads as a float (see dtoa_impl::format_buffer,
    // "Make it look like a floating-point number").
    check("momentum active: max_chars rationale format",
          ctx.trace.size() == 1 &&
              ctx.trace[0].rationale == "effective_chars=50.0 raw_chars=500");
}

// Engine-level: momentum disabled by default, even with a multi-turn
// user_turn_chars series present in the RouteContext, must be fully inert.
void test_engine_momentum_disabled_is_inert() {
    RoutePolicy policy;
    policy.candidates = {"local", "cloud"};
    policy.default_model = "local";
    policy.rules = {
        make_rule("min_chars-rule", deterministic_leaf(json{{"min_chars", 400}}), "cloud"),
    };
    // policy.momentum defaults to {enabled=false, ...} — not touched.
    RoutingPolicyEngine engine(std::move(policy), ClassifierServices{});

    RouteContext ctx;
    ctx.user_turn_chars = {100, 1000, 50};  // last turn (50) alone misses min_chars:400
    ctx.params.chars = 50;
    Decision d = engine.route(ctx, /*want_trace=*/true);
    check("momentum disabled: falls open to default despite rich history present",
          d.default_used && d.route_to == "local");
    check("momentum disabled: trace has no rationale",
          d.trace.size() == 1 && d.trace[0].rationale.empty());
}

// Engine-level: the headline anti-flapping scenario from the issue. A long
// turn escalates; momentum keeps a short follow-up on the escalated route.
void test_engine_momentum_enabled_survives_short_followup() {
    RoutePolicy policy;
    policy.candidates = {"local", "cloud"};
    policy.default_model = "local";
    policy.momentum = {/*enabled=*/true, /*attack=*/0.5, /*release=*/0.25};
    policy.rules = {
        make_rule("min_chars-rule", deterministic_leaf(json{{"min_chars", 400}}), "cloud"),
    };
    RoutingPolicyEngine engine(std::move(policy), ClassifierServices{});

    RouteContext ctx;
    ctx.user_turn_chars = {100, 1000, 50};  // effective_chars folds to 425.0
    ctx.params.chars = 50;                  // raw last turn alone would miss min_chars:400
    Decision d = engine.route(ctx, /*want_trace=*/true);
    check("momentum enabled: routes on effective length (425 >= 400), not raw (50 < 400)",
          !d.default_used && d.route_to == "cloud" && d.matched_rule == "min_chars-rule");
    // nlohmann's dump() appends ".0" to an exact-integer-valued double.
    check("momentum enabled: trace rationale carries effective/raw values",
          d.trace.size() == 1 &&
              d.trace[0].rationale == "effective_chars=425.0 raw_chars=50");
}

// The engine must degrade gracefully (not divide-by-zero / UB) when momentum
// is enabled but the RouteContext was hand-built without user_turn_chars
// (e.g. the /routing/validate debug endpoint) — folds down to a single-entry
// series equal to params.chars.
void test_engine_momentum_enabled_empty_turn_series_falls_back_to_raw() {
    RoutePolicy policy;
    policy.candidates = {"local", "cloud"};
    policy.default_model = "local";
    policy.momentum = {/*enabled=*/true, /*attack=*/1.0, /*release=*/0.3};
    policy.rules = {
        make_rule("min_chars-rule", deterministic_leaf(json{{"min_chars", 10}}), "cloud"),
    };
    RoutingPolicyEngine engine(std::move(policy), ClassifierServices{});

    RouteContext ctx;
    // user_turn_chars deliberately left empty.
    ctx.input = std::string(20, 'x');
    ctx.params.chars = ctx.input.size();
    Decision d = engine.route(ctx, false);
    check("momentum enabled + empty user_turn_chars degrades to raw params.chars",
          !d.default_used && d.route_to == "cloud");
}

}  // namespace

int main() {
    test_compute_effective_chars_single_element();
    test_compute_effective_chars_dyadic_exact();
    test_compute_effective_chars_tie_uses_attack();
    test_compute_effective_chars_degenerate_attack_release_one();
    test_compute_effective_chars_default_coefficients();
    test_compute_effective_chars_empty_is_defensive_zero();
    test_chars_condition_momentum_inactive_matches_today();
    test_chars_condition_momentum_active_overrides_raw();
    test_chars_condition_momentum_active_max_chars();
    test_engine_momentum_disabled_is_inert();
    test_engine_momentum_enabled_survives_short_followup();
    test_engine_momentum_enabled_empty_turn_series_falls_back_to_raw();

    std::printf("\n%s\n", g_failures == 0 ? "ALL MOMENTUM TESTS PASSED"
                                          : "MOMENTUM TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
