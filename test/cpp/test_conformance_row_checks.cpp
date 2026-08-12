// Self-test for the conformance corpus runner's pure row guards (#2425).
//
// The runner rejects malformed cases.jsonl rows (unknown key, missing/duplicate
// name, unknown service). These guards protect the whole corpus, so a regression
// that quietly stopped rejecting bad input would let coverage erode while CI
// stayed green. This locks the pure checks in memory — no fixtures, no second
// corpus.

#include "test_conformance_row_harness.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <set>
#include <string>

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

    const json ok = {{"name", "c"}, {"note", "n"}, {"request", json::object()},
                     {"decision", json::object()}, {"services", json::object()}};
    r.expect("all allowed keys accepted", unknown_row_keys(ok).empty());

    const json typo = {{"name", "c"}, {"request", json::object()}, {"expected", json::object()}};
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

static void test_check_case_name(TestResult& r) {
    using lemon::conformance::check_case_name;

    const std::set<std::string> seen = {"already"};

    r.expect("fresh name is ok",
             check_case_name(json{{"name", "fresh"}}, seen) == NameStatus::kOk);
    r.expect("missing name key",
             check_case_name(json::object(), seen) == NameStatus::kMissing);
    r.expect("empty name string",
             check_case_name(json{{"name", ""}}, seen) == NameStatus::kMissing);
    r.expect("duplicate name",
             check_case_name(json{{"name", "already"}}, seen) == NameStatus::kDuplicate);
}

int main() {
    TestResult r;
    std::printf("=== Conformance Row Checks Unit Tests ===\n\n");

    test_unknown_row_keys(r);
    test_unknown_service_names(r);
    test_check_case_name(r);

    std::printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
