#include "lemon/jobs/job_expr.h"

#include <cstdio>
#include <string>

using namespace lemon::jobs;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool threw(const std::string& expr, const json& ctx) {
    try {
        eval_condition(expr, ctx);
        return false;
    } catch (const JobError&) {
        return true;
    }
}

static void test_reference_resolution() {
    json ctx = {{"model", "Agents-A1"},
                {"run_v", {{"tps", 73.3}, {"ttft", 120.0}}},
                {"list", {10, 20, 30}},
                {"cfg", {{"nested", {{"deep", 5}}}}}};

    json m = resolve_refs("${model}", ctx);
    check("ref: whole-string keeps string", m.is_string() && m == "Agents-A1");
    json tps = resolve_refs("${run_v.tps}", ctx);
    check("ref: whole-string keeps number", tps.is_number() && tps.get<double>() == 73.3);
    json idx = resolve_refs("${list.1}", ctx);
    check("ref: array index", idx.is_number() && idx.get<int>() == 20);
    json deep = resolve_refs("${cfg.nested.deep}", ctx);
    check("ref: deep path", deep.get<int>() == 5);

    json interp = resolve_refs("model=${model} tps=${run_v.tps}", ctx);
    check("ref: embedded interpolates",
          interp.is_string() && interp.get<std::string>().find("model=Agents-A1") == 0
              && interp.get<std::string>().find("tps=73.3") != std::string::npos);

    json params = {{"a", "${model}"}, {"b", {{"c", "${run_v.tps}"}}}, {"d", {1, "${list.0}"}}};
    json out = resolve_refs(params, ctx);
    check("ref: recurses objects/arrays",
          out["a"] == "Agents-A1" && out["b"]["c"].get<double>() == 73.3
              && out["d"][1].get<int>() == 10);

    check("ref: missing path throws", threw("${nope.missing}", ctx));
    bool caught = false;
    try { resolve_refs("${also.missing}", ctx); } catch (const JobError&) { caught = true; }
    check("ref: resolve_refs throws on missing", caught);
}

static void test_arithmetic_and_precedence() {
    json c = json::object();
    check("expr: add", eval_condition("2 + 3 == 5", c));
    check("expr: mul before add", eval_condition("2 + 3 * 4 == 14", c));
    check("expr: parens", eval_condition("(2 + 3) * 4 == 20", c));
    check("expr: division", eval_condition("10 / 4 == 2.5", c));
    check("expr: unary minus", eval_condition("-3 + 5 == 2", c));
    check("expr: div by zero throws", threw("1 / 0", c));
    check("expr: ratio gate", eval_condition("898.8 / 813.4 > 1.05", c));
}

static void test_comparison_and_boolean() {
    json c = {{"a", 73.3}, {"b", 55.8}, {"backend", "vulkan"}, {"ok", true}, {"empty", ""}};
    check("cmp: numeric >", eval_condition("${a} > ${b}", c));
    check("cmp: numeric >= equal", eval_condition("5 >= 5", c));
    check("cmp: string ==", eval_condition("${backend} == 'vulkan'", c));
    check("cmp: string !=", eval_condition("${backend} != 'rocm'", c));
    check("cmp: string lexicographic", eval_condition("'abc' < 'abd'", c));
    check("bool: and", eval_condition("${a} > ${b} && ${ok}", c));
    check("bool: or short of false", eval_condition("${a} < ${b} || ${backend} == 'vulkan'", c));
    check("bool: not", eval_condition("!(${a} < ${b})", c));
    check("bool: precedence and over or", eval_condition("true || false && false", c));
    check("cmp: order-compare mismatched types throws", threw("${backend} < 5", c));
}

static void test_truthiness_and_edges() {
    json c = {{"zero", 0}, {"nonzero", 3}, {"emptystr", ""}, {"str", "x"},
              {"t", true}, {"f", false}, {"nul", nullptr}, {"arr", json::array()},
              {"arr2", {1}}};
    check("truthy: number 0 false", !eval_condition("${zero}", c));
    check("truthy: number nonzero true", eval_condition("${nonzero}", c));
    check("truthy: empty string false", !eval_condition("${emptystr}", c));
    check("truthy: string true", eval_condition("${str}", c));
    check("truthy: null false", !eval_condition("${nul}", c));
    check("truthy: empty array false", !eval_condition("${arr}", c));
    check("truthy: nonempty array true", eval_condition("${arr2}", c));
    check("edge: empty expression is true", eval_condition("", c));
    check("edge: trailing tokens throw", threw("1 2", c));
    check("edge: unexpected char throws", threw("1 @ 2", c));
    check("edge: unterminated ref throws", threw("${a", c));
}

static void test_syntax_check() {
    check("syntax: valid passes", check_expression_syntax("${a} > 1 && ${b} == 'x'").empty());
    check("syntax: unresolved ref deferred", check_expression_syntax("${x.y.z} + 1").empty());
    check("syntax: type mismatch deferred", check_expression_syntax("${backend} < 5").empty());
    check("syntax: incomplete rejected", !check_expression_syntax("1 +").empty());
    check("syntax: unmatched paren rejected", !check_expression_syntax("(true").empty());
    check("syntax: chained comparison rejected", !check_expression_syntax("1 < 2 < 3").empty());
    check("syntax: trailing tokens rejected", !check_expression_syntax("1 2").empty());
    check("syntax: bad char rejected", !check_expression_syntax("1 @ 2").empty());
    check("syntax: empty ok", check_expression_syntax("").empty());
}

int main() {
    test_reference_resolution();
    test_arithmetic_and_precedence();
    test_comparison_and_boolean();
    test_truthiness_and_edges();
    test_syntax_check();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
