// Back-compat conformance corpus runner (#2425).
//
// Replays every frozen golden case under test/conformance/routing/ through the
// real routing engine and asserts the emitted Decision (via the production
// route_decision_to_json serializer) matches the frozen expectation exactly.
// Any drift is a back-compat violation.
//
// Deterministic corpus only: keywords_any / regex / min_chars / metadata and
// first-match / fail-open behavior reproduce byte-for-byte with no model
// backend, so an empty ClassifierServices is sufficient.

#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CONFORMANCE_CORPUS_DIR
#define CONFORMANCE_CORPUS_DIR "test/conformance/routing"
#endif

namespace fs = std::filesystem;

using lemon::Decision;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::json;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static json load_json_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open " + path.string());
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

static std::vector<fs::path> find_case_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().filename() == "cases.jsonl") {
            dirs.push_back(entry.path().parent_path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

static void run_case_dir(const fs::path& case_dir, const fs::path& root) {
    const std::string rel = fs::relative(case_dir, root).generic_string();

    RoutePolicy policy;
    try {
        policy = lemon::parse_route_policy_collection(load_json_file(case_dir / "policy.json"));
    } catch (const std::exception& e) {
        check(rel + ": policy.json parses", false);
        std::printf("  %s\n", e.what());
        return;
    }
    RoutingPolicyEngine engine(std::move(policy), lemon::ClassifierServices{});

    std::ifstream cases(case_dir / "cases.jsonl");
    if (!cases) {
        check(rel + ": cases.jsonl opens", false);
        return;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(cases, line)) {
        ++line_no;
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        json row = json::parse(line);
        const std::string name =
            rel + "/" + row.value("name", "line-" + std::to_string(line_no));

        const json& request = row["request"];
        const bool want_trace = request.value("route_trace", false);
        Decision decision = engine.route(
            lemon::build_route_context(request, request.value("model", "")), want_trace);
        const json produced = lemon::route_decision_to_json(decision);
        const json& expected = row.at("decision");

        const bool ok = produced == expected;
        check(name, ok);
        if (!ok) {
            std::printf("  expected: %s\n", expected.dump().c_str());
            std::printf("  produced: %s\n", produced.dump().c_str());
        }
    }
}

int main() {
    const fs::path root = CONFORMANCE_CORPUS_DIR;
    if (!fs::is_directory(root)) {
        std::printf("[FAIL] conformance corpus dir missing: %s\n", root.string().c_str());
        return 1;
    }

    const std::vector<fs::path> case_dirs = find_case_dirs(root);
    if (case_dirs.empty()) {
        std::printf("[FAIL] no cases.jsonl found under %s\n", root.string().c_str());
        return 1;
    }
    for (const auto& case_dir : case_dirs) {
        run_case_dir(case_dir, root);
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL CONFORMANCE CASES PASSED"
                                          : "CONFORMANCE CASES FAILED");
    return g_failures == 0 ? 0 : 1;
}
