// Unit tests for the Lemonade Router deterministic leaf conditions (#2380).
//
// Covers keywords_any/keywords_all, regex, min_chars/max_chars, has_tools/
// has_images, and metadata against the frozen v1 semantics in
// route_policy.schema.json: case-insensitive (ASCII) substring, ECMAScript
// regex, inclusive UTF-8-byte length bounds, and metadata equals/any/exists
// (scalar vs comma-encoded list, missing-key => exists:false). Also exercises
// malformed-config rejection and the registry's multi-key implicit-all wiring.
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       test/cpp/test_routing_policy_deterministic.cpp src/cpp/server/routing_policy.cpp \
//       -o test_routing_policy_deterministic

#include "lemon/routing_policy.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using lemon::ClassifierPtr;
using lemon::ClassifierServices;
using lemon::ConditionPtr;
using lemon::EvalContext;
using lemon::LeafFactory;
using lemon::NamedLeafFactories;
using lemon::RouteContext;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

namespace {

// Build a one-shot deterministic leaf Condition for the single-key leaf `{op:value}`.
// Mirrors what make_leaf_factory hands each deterministic factory.
ConditionPtr build_leaf(const std::string& op, const json& value) {
    NamedLeafFactories factories = lemon::make_deterministic_leaf_factories();
    auto it = factories.find(op);
    if (it == factories.end()) {
        throw std::invalid_argument("no factory for op: " + op);
    }
    json leaf = json::object();
    leaf[op] = value;
    return it->second(leaf);
}

// Evaluate a single-key leaf against a request, no trace.
bool eval_leaf(const std::string& op, const json& value, const RouteContext& req) {
    ClassifierServices services;
    ConditionPtr cond = build_leaf(op, value);
    EvalContext ctx{req, services};
    return cond->evaluate(ctx);
}

RouteContext make_request(const std::string& input) {
    RouteContext req;
    req.input = input;
    req.params.chars = input.size();
    return req;
}

bool throws_invalid(const std::string& op, const json& value) {
    try {
        build_leaf(op, value);
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void test_keywords_any() {
    RouteContext req = make_request("Please write a Python function for me");
    check("keywords_any matches one present",
          eval_leaf("keywords_any", json::array({"function", "stack trace"}), req));
    check("keywords_any no match",
          !eval_leaf("keywords_any", json::array({"banana", "kotlin"}), req));
}

void test_keywords_all() {
    RouteContext req = make_request("def foo(): compile and run");
    check("keywords_all all present",
          eval_leaf("keywords_all", json::array({"def ", "compile"}), req));
    check("keywords_all one missing",
          !eval_leaf("keywords_all", json::array({"def ", "missing"}), req));
}

void test_case_insensitive() {
    RouteContext req = make_request("STACK TRACE in the logs");
    check("keywords case-insensitive (lower needle, upper haystack)",
          eval_leaf("keywords_any", json::array({"stack trace"}), req));
    RouteContext req2 = make_request("a quiet sentence");
    check("keywords case-insensitive (mixed)",
          eval_leaf("keywords_any", json::array({"QUIET"}), req2));
}

void test_regex() {
    RouteContext fence = make_request("here is code:\n```python\nx=1\n```");
    check("regex ECMAScript fenced block",
          eval_leaf("regex", "```[a-z]*", fence));
    RouteContext plain = make_request("just a plain sentence");
    check("regex no match", !eval_leaf("regex", "```[a-z]*", plain));
    RouteContext digits = make_request("order 12345 please");
    check("regex digit class", eval_leaf("regex", "\\d{3,}", digits));
}

void test_regex_input_cap() {
    // A matching pattern on an oversized input is treated as a non-match (fail-safe
    // against a hung worker), while just under the cap still matches.
    RouteContext under = make_request("x" + std::string((1u << 20) - 1, 'a'));  // 1 MiB total
    check("regex matches at/under 1 MiB cap", eval_leaf("regex", "x", under));
    RouteContext over = make_request("x" + std::string(1u << 20, 'a'));  // 1 MiB + 1
    check("regex over 1 MiB cap => non-match", !eval_leaf("regex", "x", over));
}

void test_chars() {
    RouteContext req = make_request("12345");  // 5 bytes
    check("min_chars inclusive lower boundary", eval_leaf("min_chars", 5, req));
    check("min_chars below threshold", !eval_leaf("min_chars", 6, req));
    check("max_chars inclusive upper boundary", eval_leaf("max_chars", 5, req));
    check("max_chars above threshold", !eval_leaf("max_chars", 4, req));
}

void test_chars_utf8_bytes() {
    // "é" is 2 UTF-8 bytes; "café" => 5 bytes, not 4 code points.
    RouteContext req = make_request("caf\xC3\xA9");
    check("min_chars counts UTF-8 bytes (>=5 true)", eval_leaf("min_chars", 5, req));
    check("min_chars counts UTF-8 bytes (>=6 false)", !eval_leaf("min_chars", 6, req));
    check("max_chars counts UTF-8 bytes (<=4 false)", !eval_leaf("max_chars", 4, req));
}

void test_has_features() {
    RouteContext req = make_request("hi");
    req.params.has_tools = true;
    req.params.has_images = false;
    check("has_tools true matches", eval_leaf("has_tools", true, req));
    check("has_tools:false does not match when tools present",
          !eval_leaf("has_tools", false, req));
    check("has_images:false matches when images absent",
          eval_leaf("has_images", false, req));
    check("has_images:true does not match when absent",
          !eval_leaf("has_images", true, req));
}

void test_metadata_equals() {
    RouteContext req = make_request("x");
    req.metadata["task_class"] = "code";
    check("metadata equals exact",
          eval_leaf("metadata", json{{"key", "task_class"}, {"equals", "code"}}, req));
    check("metadata equals case-sensitive mismatch",
          !eval_leaf("metadata", json{{"key", "task_class"}, {"equals", "Code"}}, req));
    check("metadata equals missing key => false",
          !eval_leaf("metadata", json{{"key", "absent"}, {"equals", "code"}}, req));
}

void test_metadata_any() {
    RouteContext scalar = make_request("x");
    scalar.metadata["topic"] = "math";
    check("metadata any scalar intersects",
          eval_leaf("metadata", json{{"key", "topic"}, {"any", json::array({"code", "math"})}},
                    scalar));

    RouteContext list = make_request("x");
    list.metadata["topic"] = " code , math ";  // comma-encoded, with whitespace
    check("metadata any list intersects (trimmed tokens)",
          eval_leaf("metadata", json{{"key", "topic"}, {"any", json::array({"math"})}}, list));
    check("metadata any no intersection",
          !eval_leaf("metadata", json{{"key", "topic"}, {"any", json::array({"prose"})}}, list));
}

void test_metadata_exists() {
    RouteContext present = make_request("x");
    present.metadata["consent"] = "granted";
    check("metadata exists:true when present",
          eval_leaf("metadata", json{{"key", "consent"}, {"exists", true}}, present));
    check("metadata exists:false when present => no match",
          !eval_leaf("metadata", json{{"key", "consent"}, {"exists", false}}, present));

    RouteContext absent = make_request("x");
    check("metadata exists:false when absent",
          eval_leaf("metadata", json{{"key", "consent"}, {"exists", false}}, absent));
    check("metadata exists:true when absent => no match",
          !eval_leaf("metadata", json{{"key", "consent"}, {"exists", true}}, absent));

    RouteContext empty = make_request("x");
    empty.metadata["consent"] = "";  // empty value counts as absent
    check("metadata empty value => exists:false matches",
          eval_leaf("metadata", json{{"key", "consent"}, {"exists", false}}, empty));

    RouteContext blank = make_request("x");
    blank.metadata["consent"] = "   \t";  // whitespace-only counts as absent
    check("metadata whitespace-only value => exists:false matches",
          eval_leaf("metadata", json{{"key", "consent"}, {"exists", false}}, blank));
    check("metadata whitespace-only value => exists:true no match",
          !eval_leaf("metadata", json{{"key", "consent"}, {"exists", true}}, blank));
}

void test_rejections() {
    check("empty keywords_any rejected", throws_invalid("keywords_any", json::array()));
    check("empty keywords_all rejected", throws_invalid("keywords_all", json::array()));
    check("empty keyword item rejected",
          throws_invalid("keywords_any", json::array({""})));
    check("non-string keyword rejected",
          throws_invalid("keywords_any", json::array({1})));
    check("invalid regex rejected", throws_invalid("regex", "[unterminated"));
    check("non-string regex rejected", throws_invalid("regex", 5));
    check("negative min_chars rejected", throws_invalid("min_chars", -1));
    check("non-integer max_chars rejected", throws_invalid("max_chars", 1.5));
    check("non-bool has_tools rejected", throws_invalid("has_tools", "yes"));
    check("metadata missing key rejected",
          throws_invalid("metadata", json{{"equals", "code"}}));
    check("metadata zero comparators rejected",
          throws_invalid("metadata", json{{"key", "k"}}));
    check("metadata two comparators rejected",
          throws_invalid("metadata",
                         json{{"key", "k"}, {"equals", "a"}, {"exists", true}}));
    check("metadata empty any rejected",
          throws_invalid("metadata", json{{"key", "k"}, {"any", json::array()}}));
    check("metadata empty key rejected",
          throws_invalid("metadata", json{{"key", ""}, {"exists", true}}));
    check("metadata empty any item rejected",
          throws_invalid("metadata", json{{"key", "k"}, {"any", json::array({""})}}));
}

void test_regex_redos_rejected() {
    // Nested unbounded quantifiers (catastrophic backtracking) rejected at build.
    check("regex (a+)+ rejected", throws_invalid("regex", "(a+)+"));
    check("regex (a*)* rejected", throws_invalid("regex", "(a*)*"));
    check("regex (.*)+ rejected", throws_invalid("regex", "(.*)+"));
    check("regex (\\d+){2,} rejected", throws_invalid("regex", "(\\d+){2,}"));
    check("regex ((a+)+)+ rejected", throws_invalid("regex", "((a+)+)+"));
    check("regex empty pattern rejected", throws_invalid("regex", ""));
    // Wrapper group must not hide the nested unbounded quantifier.
    check("regex ((a+))+ rejected", throws_invalid("regex", "((a+))+"));
    check("regex (a(b+))+ rejected", throws_invalid("regex", "(a(b+))+"));

    // Safe patterns still accepted (no nested unbounded quantifier).
    auto accepts = [](const json& v) {
        try {
            build_leaf("regex", v);
        } catch (...) {
            return false;
        }
        return true;
    };
    check("regex (ab)+ accepted", accepts("(ab)+"));
    check("regex (\\d+) accepted (inner only)", accepts("(\\d+)"));
    check("regex (a+){1,3} accepted (bounded outer)", accepts("(a+){1,3}"));
    check("regex (a{1,3})+ accepted (bounded inner)", accepts("(a{1,3})+"));
    check("regex ```[a-z]* accepted", accepts("```[a-z]*"));
    check("regex \\$\\d+ accepted", accepts("\\$\\d+"));
}

void test_trace_emitted() {
    RouteContext req = make_request("write a function");
    ClassifierServices services;
    ConditionPtr cond = build_leaf("keywords_any", json::array({"function"}));
    EvalContext ctx{req, services};
    ctx.want_trace = true;
    const bool result = cond->evaluate(ctx);
    check("trace: result true", result);
    check("trace: one entry", ctx.trace.size() == 1);
    check("trace: condition name", !ctx.trace.empty() && ctx.trace[0].condition == "keywords_any");
    check("trace: no score on deterministic leaf",
          !ctx.trace.empty() && !ctx.trace[0].score.has_value());
}

// The registry isolates each top-level key into its own single-key leaf and
// ANDs multiple deterministic ops (implicit all). Verify a two-op leaf.
void test_registry_implicit_all() {
    std::map<std::string, ClassifierPtr> classifiers;
    LeafFactory factory =
        lemon::make_leaf_factory(classifiers, lemon::make_deterministic_leaf_factories());

    json leaf = json::object();
    leaf["keywords_any"] = json::array({"function"});
    leaf["min_chars"] = 5;
    ConditionPtr cond = factory(leaf);

    ClassifierServices services;
    RouteContext both = make_request("write a function please");  // matches both
    EvalContext ctx_both{both, services};
    check("implicit-all both true", cond->evaluate(ctx_both));

    RouteContext short_req = make_request("fn");  // no keyword + below min_chars
    EvalContext ctx_short{short_req, services};
    check("implicit-all one false => false", !cond->evaluate(ctx_short));
}

}  // namespace

int main() {
    test_keywords_any();
    test_keywords_all();
    test_case_insensitive();
    test_regex();
    test_regex_input_cap();
    test_chars();
    test_chars_utf8_bytes();
    test_has_features();
    test_metadata_equals();
    test_metadata_any();
    test_metadata_exists();
    test_rejections();
    test_regex_redos_rejected();
    test_trace_emitted();
    test_registry_implicit_all();

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
