// Self-test for the conformance corpus runner's pure row guards (#2425).
//
// The runner rejects malformed cases.jsonl rows (unknown key, missing/duplicate
// name, unknown service) and a policies.json that declares a policy name twice.
// These guards protect the whole corpus, so a regression that quietly stopped
// rejecting bad input would let coverage erode while CI stayed green. This locks
// the pure checks in memory — no fixtures, no second corpus.

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

static void test_duplicate_policy_names(TestResult& r) {
    using lemon::conformance::duplicate_policy_names;

    r.expect("distinct names accepted",
             duplicate_policy_names(R"({"a": {}, "b": {}})").empty());

    const std::vector<std::string> dup = duplicate_policy_names(R"({"a": {}, "b": {}, "a": {}})");
    r.expect("repeated name reported", dup.size() == 1 && dup.front() == "a");

    r.expect("a name repeated twice is reported once",
             duplicate_policy_names(R"({"a": {}, "a": {}, "a": {}})").size() == 1);

    // Only the policy names are checked; keys inside a policy are the parser's business.
    r.expect("repeated key inside a policy ignored",
             duplicate_policy_names(R"({"a": {"x": 1, "x": 2}})").empty());

    r.expect("unparsable text reports nothing", duplicate_policy_names("{").empty());
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
    const json entry = {{"condition", "classifier:topic"}, {"result", true}, {"score", 0.5}};
    json near = entry;
    near["score"] = 0.5 + 1e-13;
    r.expect("score within tolerance matches",
             compare_decision(with_trace(entry), with_trace(near)).empty());

    json far = entry;
    far["score"] = 0.5 + 1e-6;
    r.expect("score beyond tolerance reported",
             says(compare_decision(with_trace(entry), with_trace(far)),
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
    test_duplicate_policy_names(r);
    test_name_chars_ok(r);
    test_check_case_name(r);
    test_compare_decision(r);
    test_compare_trace(r);

    std::printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
