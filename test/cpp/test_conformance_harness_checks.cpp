// Self-test for the conformance corpus runner's pure row guards (#2425).
//
// The runner rejects malformed cases.jsonl rows (unknown key, missing/duplicate
// name, unknown service) and any corpus file that declares the same key twice
// inside one object. These guards protect the whole corpus, so a regression that
// quietly stopped rejecting bad input would let coverage erode while CI stayed
// green. This locks the pure checks in memory — no fixtures, no second corpus.

#include "conformance_decision_compare.h"
#include "conformance_row_checks.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using lemon::conformance::NameStatus;
using nlohmann::json;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void expect(const std::string& name, bool ok) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
        if (ok) ++passed; else ++failed;
    }
};

static void test_unknown_row_keys(TestResult& r) {
    using lemon::conformance::unknown_row_keys;

    const json ok = {{"case_name", "c"}, {"policy_name", "p"}, {"note", "n"}, {"request", json::object()},
                     {"decision", json::object()}, {"services", json::object()}};
    r.expect("all allowed keys accepted", unknown_row_keys(ok).empty());

    const json typo = {{"case_name", "c"}, {"request", json::object()}, {"expected", json::object()}};
    const std::vector<std::string> bad = unknown_row_keys(typo);
    r.expect("typo'd key rejected", bad.size() == 1 && bad.front() == "expected");

    r.expect("non-object has no keys", unknown_row_keys(json::array()).empty());
}

static void test_unknown_service_names(TestResult& r) {
    using lemon::conformance::unknown_service_names;

    const json ok = {{"embed", json::object()}, {"run_classifier", json::object()},
                     {"chat", json::object()}};
    r.expect("all allowed services accepted", unknown_service_names(ok).empty());

    const json bad = {{"embed", json::object()}, {"rerank", json::object()}};
    const std::vector<std::string> unknown = unknown_service_names(bad);
    r.expect("unknown service rejected", unknown.size() == 1 && unknown.front() == "rerank");
}

static void test_mistyped_request_fields(TestResult& r) {
    using lemon::conformance::mistyped_request_fields;

    r.expect("well-typed request accepted",
             mistyped_request_fields(json{{"model", "router"}, {"route_trace", true}}).empty());
    r.expect("both fields may be absent", mistyped_request_fields(json::object()).empty());

    // value() throws on these, which would abort the run instead of failing the case.
    const std::vector<std::string> trace =
        mistyped_request_fields(json{{"model", "router"}, {"route_trace", "yes"}});
    r.expect("non-boolean route_trace rejected",
             trace.size() == 1 && trace.front() == "route_trace must be a boolean");

    const std::vector<std::string> model = mistyped_request_fields(json{{"model", 5}});
    r.expect("non-string model rejected",
             model.size() == 1 && model.front() == "model must be a string");

    r.expect("both mistyped fields reported",
             mistyped_request_fields(json{{"model", 5}, {"route_trace", 1}}).size() == 2);
    r.expect("non-object request has no fields to check",
             mistyped_request_fields(json::array()).empty());
}

static void test_duplicate_object_keys(TestResult& r) {
    using lemon::conformance::duplicate_object_keys;

    r.expect("distinct names accepted",
             duplicate_object_keys(R"({"a": {}, "b": {}})").empty());

    const std::vector<std::string> dup = duplicate_object_keys(R"({"a": {}, "b": {}, "a": {}})");
    r.expect("repeated name reported", dup.size() == 1 && dup.front() == "a");

    r.expect("a name repeated twice is reported once",
             duplicate_object_keys(R"({"a": {}, "a": {}, "a": {}})").size() == 1);

    const std::vector<std::string> nested = duplicate_object_keys(R"({"a": {"x": 1, "x": 2}})");
    r.expect("repeated key inside a policy reported by path",
             nested.size() == 1 && nested.front() == "a.x");

    const std::vector<std::string> deep =
        duplicate_object_keys(R"({"a": {"rules": [{"decision": 1, "decision": 2}]}})");
    r.expect("repeated key inside an array element reported",
             deep.size() == 1 && deep.front() == "a.rules.decision");

    // The same name under two different parents is two distinct keys.
    r.expect("same name in sibling objects accepted",
             duplicate_object_keys(R"({"a": {"x": 1}, "b": {"x": 2}})").empty());

    // A cases.jsonl row is checked the same way.
    const std::vector<std::string> row =
        duplicate_object_keys(R"({"case_name": "c", "request": {"model": "m", "model": "n"}})");
    r.expect("repeated key in a case row reported",
             row.size() == 1 && row.front() == "request.model");

    r.expect("unparsable text reports nothing", duplicate_object_keys("{").empty());
}

static void test_name_chars_ok(TestResult& r) {
    using lemon::conformance::name_chars_ok;

    r.expect("plain name accepted", name_chars_ok("conditions_char_bounds"));
    r.expect("tab rejected", !name_chars_ok("a\tb"));
    r.expect("newline rejected", !name_chars_ok("a\nb"));
}

static void test_check_case_name(TestResult& r) {
    using lemon::conformance::check_case_name;

    const std::set<std::string> seen = {"already"};

    r.expect("fresh name is ok",
             check_case_name(json{{"case_name", "fresh"}}, seen) == NameStatus::kOk);
    r.expect("missing name key",
             check_case_name(json::object(), seen) == NameStatus::kMissing);
    r.expect("empty name string",
             check_case_name(json{{"case_name", ""}}, seen) == NameStatus::kMissing);
    r.expect("non-string name",
             check_case_name(json{{"case_name", 123}}, seen) == NameStatus::kNotString);
    r.expect("duplicate name",
             check_case_name(json{{"case_name", "already"}}, seen) == NameStatus::kDuplicate);
    r.expect("name with a slash rejected",
             check_case_name(json{{"case_name", "a/b"}}, seen) == NameStatus::kInvalidChars);
    r.expect("name with a colon rejected",
             check_case_name(json{{"case_name", "a:b"}}, seen) == NameStatus::kInvalidChars);
    r.expect("name with a space rejected",
             check_case_name(json{{"case_name", "a b"}}, seen) == NameStatus::kInvalidChars);
    r.expect("dots, dashes and underscores allowed",
             check_case_name(json{{"case_name", "min_chars-inclusive.boundary"}}, seen) == NameStatus::kOk);
}

static json decision(const json& extra = json::object()) {
    json d = {{"version", "1"},      {"route_to", "LocalLLM"}, {"matched_rule", "rule-1"},
              {"default_used", false}, {"outputs", json::object()}};
    for (auto it = extra.begin(); it != extra.end(); ++it) d[it.key()] = it.value();
    return d;
}

static json with_trace(const json& entry) {
    json d = decision();
    d["trace"] = json::array({entry});
    return d;
}

static bool says(const std::vector<std::string>& out, const std::string& needle) {
    return out.size() == 1 && out.front().find(needle) != std::string::npos;
}

static void test_compare_decision(TestResult& r) {
    using lemon::conformance::compare_decision;
    r.expect("identical decisions match", compare_decision(decision(), decision()).empty());
    r.expect("changed value reported",
             says(compare_decision(decision(), decision({{"route_to", "CloudLLM"}})),
                  "route_to: expected"));

    // Drift between route_decision_to_json and the comparison schema.
    r.expect("unexpected field reported",
             says(compare_decision(decision(), decision({{"confidence", 0.5}})),
                  "confidence: unexpected field"));

    // A known field on one side only is a case failure, not schema drift.
    r.expect("field on one side only reported",
             says(compare_decision(decision({{"trace", json::array()}}), decision()),
                  "trace: present in the expected decision only"));

    json without_route = decision();
    without_route.erase("route_to");
    r.expect("required field missing from both reported",
             says(compare_decision(without_route, without_route),
                  "route_to: missing from both sides"));
}

static void test_compare_trace(TestResult& r) {
    using lemon::conformance::compare_decision;
    const std::set<std::string> computed = {"classifier:topic"};
    const json entry = {{"condition", "classifier:topic"}, {"result", true}, {"score", 0.5}};
    json near = entry;
    near["score"] = 0.5 + 1e-13;
    r.expect("computed score within tolerance matches",
             compare_decision(with_trace(entry), with_trace(near), computed).empty());

    json far = entry;
    far["score"] = 0.5 + 1e-6;
    r.expect("computed score beyond tolerance reported",
             says(compare_decision(with_trace(entry), with_trace(far), computed),
                  "trace[0].score: expected"));

    // The margin belongs to the cosine, not to the trace. A score the case copied from
    // a stub answer is exact, so the same delta that the computed score absorbs must
    // fail here.
    r.expect("stub-fed score is compared exactly",
             says(compare_decision(with_trace(entry), with_trace(near), {}),
                  "trace[0].score: expected"));

    const json other = {{"condition", "classifier:other"}, {"result", true}, {"score", 0.5}};
    json other_near = other;
    other_near["score"] = 0.5 + 1e-13;
    r.expect("a condition outside the computed set gets no margin",
             says(compare_decision(with_trace(other), with_trace(other_near), computed),
                  "trace[0].score: expected"));

    r.expect("trace length mismatch reported",
             says(compare_decision(with_trace(entry), decision({{"trace", json::array()}})),
                  "trace: expected 1 entries, produced 0"));

    json with_rationale = entry;
    with_rationale["rationale"] = "because";
    r.expect("rationale is compared",
             says(compare_decision(with_trace(with_rationale), with_trace(entry)),
                  "trace[0].rationale: present in the expected decision only"));

    json unknown_entry = entry;
    unknown_entry["weight"] = 1;
    r.expect("unexpected trace field reported",
             says(compare_decision(with_trace(unknown_entry), with_trace(entry)),
                  "trace[0].weight: unexpected field"));
}

int main() {
    TestResult r;
    std::printf("=== Conformance Row Checks Unit Tests ===\n\n");

    test_unknown_row_keys(r);
    test_unknown_service_names(r);
    test_mistyped_request_fields(r);
    test_duplicate_object_keys(r);
    test_name_chars_ok(r);
    test_check_case_name(r);
    test_compare_decision(r);
    test_compare_trace(r);

    std::printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
