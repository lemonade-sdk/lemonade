// Back-compat conformance corpus runner (#2425).
//
// Replays every golden case under test/conformance/routing/ through the real
// routing engine and asserts the emitted Decision (via the production
// route_decision_to_json serializer) equals the recorded expectation: same
// fields, same values, no tolerance. Any drift is a back-compat violation.
//
// Deterministic corpus only: keywords_any / regex / min_chars / metadata and
// first-match / fail-open behavior reproduce exactly with no model backend, so
// an empty ClassifierServices is sufficient.

#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
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

// fs::relative resolves the path, so it throws on entries the corpus should reject
// (a symlink loop, for one). Labels must survive those.
static std::string rel_label(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    const fs::path rel = fs::relative(path, root, ec);
    return (ec || rel.empty()) ? path.lexically_relative(root).generic_string()
                               : rel.generic_string();
}

struct DirEntries {
    std::vector<fs::path> dirs;
    std::vector<fs::path> non_dirs;
};

static DirEntries list_entries(const fs::path& dir, std::error_code& ec) {
    DirEntries entries;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        auto& bucket = it->is_directory(ec) ? entries.dirs : entries.non_dirs;
        bucket.push_back(it->path());
    }
    std::sort(entries.dirs.begin(), entries.dirs.end());
    std::sort(entries.non_dirs.begin(), entries.non_dirs.end());
    return entries;
}

// Corpus layout is exactly routing/<version>/<case>/{policy.json,cases.jsonl}. A version
// dir holding anything but case dirs, a case dir missing either file or holding a nested
// subdirectory, and a dir that cannot be read are all hard failures: silent coverage loss
// / drifted layout otherwise.
static std::vector<fs::path> find_case_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    // Files directly under the root are docs (README.md), not corpus content.
    const DirEntries root_entries = list_entries(root, ec);
    if (ec) {
        check(root.generic_string() + ": is readable", false);
        std::printf("  %s\n", ec.message().c_str());
        return dirs;
    }
    for (const auto& version : root_entries.dirs) {
        std::error_code vec;
        const DirEntries version_entries = list_entries(version, vec);
        if (vec) {
            check(rel_label(version, root) + ": is readable", false);
            std::printf("  %s\n", vec.message().c_str());
            continue;
        }
        for (const auto& stray : version_entries.non_dirs) {
            check(rel_label(stray, root) + ": is a case directory", false);
        }
        for (const auto& case_dir : version_entries.dirs) {
            const std::string rel = rel_label(case_dir, root);

            std::error_code policy_ec;
            std::error_code cases_ec;
            const bool has_policy = fs::exists(case_dir / "policy.json", policy_ec);
            const bool has_cases = fs::exists(case_dir / "cases.jsonl", cases_ec);
            std::error_code sec;
            const DirEntries nested = list_entries(case_dir, sec);

            bool ok = true;
            if (policy_ec || cases_ec) {
                check(rel + ": entries are readable", false);
                std::printf("  %s\n", (policy_ec ? policy_ec : cases_ec).message().c_str());
                ok = false;
            } else if (!has_policy || !has_cases) {
                const std::string missing = (!has_policy && !has_cases)
                                                ? "policy.json and cases.jsonl"
                                                : (!has_policy ? "policy.json" : "cases.jsonl");
                check(rel + ": has policy.json + cases.jsonl", false);
                std::printf("  missing %s\n", missing.c_str());
                ok = false;
            }
            if (sec) {
                check(rel + ": is readable", false);
                std::printf("  %s\n", sec.message().c_str());
                ok = false;
            } else if (!nested.dirs.empty()) {
                check(rel + ": is a leaf (no subdirectories)", false);
                ok = false;
            }
            if (ok) {
                dirs.push_back(case_dir);
            }
        }
    }
    return dirs;
}

static void report_mismatch(const json& expected, const json& produced) {
    std::printf("  expected: %s\n", expected.dump().c_str());
    std::printf("  produced: %s\n", produced.dump().c_str());
    const json patch = json::diff(expected, produced);
    std::printf("  %zu field(s) differ:\n", patch.size());
    for (const auto& op : patch) {
        const auto ptr = json::json_pointer(op.value("path", ""));
        const std::string exp = expected.contains(ptr) ? expected.at(ptr).dump() : "<absent>";
        const std::string prod = produced.contains(ptr) ? produced.at(ptr).dump() : "<absent>";
        std::printf("    %s: expected: %s, produced: %s\n", op.value("path", "").c_str(),
                    exp.c_str(), prod.c_str());
    }
}

static int run_case_dir(const fs::path& case_dir, const fs::path& root) {
    const std::string rel = rel_label(case_dir, root);

    RoutePolicy policy;
    try {
        const json policy_json = load_json_file(case_dir / "policy.json");
        const std::string directory_version = case_dir.parent_path().filename().string();
        if (!policy_json.contains("version") || !policy_json["version"].is_string() ||
            policy_json["version"].get<std::string>() != directory_version) {
            check(rel + ": policy version matches schema-major directory", false);
            return 0;
        }
        policy = lemon::parse_route_policy_collection(policy_json);
    } catch (const std::exception& e) {
        check(rel + ": policy.json parses", false);
        std::printf("  %s\n", e.what());
        return 0;
    }
    std::optional<RoutingPolicyEngine> engine;
    try {
        engine.emplace(std::move(policy), lemon::ClassifierServices{});
    } catch (const std::exception& e) {
        check(rel + ": policy compiles", false);
        std::printf("  %s\n", e.what());
        return 0;
    }

    std::ifstream cases(case_dir / "cases.jsonl");
    if (!cases) {
        check(rel + ": cases.jsonl opens", false);
        return 0;
    }

    int executed = 0;
    std::string line;
    int line_no = 0;
    std::set<std::string> seen_names;
    static const std::set<std::string> kAllowedRowKeys = {"name", "note", "request", "decision"};
    while (std::getline(cases, line)) {
        ++line_no;
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        json row;
        try {
            row = json::parse(line);
        } catch (const std::exception& e) {
            check(rel + ": cases.jsonl line " + std::to_string(line_no) + " parses", false);
            std::printf("  %s\n", e.what());
            continue;
        }
        if (!row.is_object() || !row.contains("request") || !row.contains("decision") ||
            !row["request"].is_object() || !row["decision"].is_object()) {
            check(rel + ": cases.jsonl line " + std::to_string(line_no) +
                      " has object request+decision",
                  false);
            continue;
        }
        for (auto it = row.begin(); it != row.end(); ++it) {
            if (kAllowedRowKeys.count(it.key()) == 0) {
                check(rel + ": cases.jsonl line " + std::to_string(line_no) +
                          " unknown key '" + it.key() + "'",
                      false);
            }
        }

        const std::string case_name = row.value("name", "");
        if (case_name.empty()) {
            check(rel + ": cases.jsonl line " + std::to_string(line_no) + " has a name", false);
            continue;
        }
        if (!seen_names.insert(case_name).second) {
            check(rel + ": cases.jsonl line " + std::to_string(line_no) +
                      " duplicate case name '" + case_name + "'",
                  false);
            continue;
        }
        const std::string name = rel + "/" + case_name;

        const json& request = row.at("request");
        const bool want_trace = request.value("route_trace", false);
        Decision decision = engine->route(
            lemon::build_route_context(request, request.value("model", "")), want_trace);
        const json produced = lemon::route_decision_to_json(decision);
        const json& expected = row.at("decision");

        const bool ok = produced == expected;
        check(name, ok);
        if (!ok) {
            report_mismatch(expected, produced);
        }
        ++executed;
    }

    check(rel + ": cases.jsonl has at least one case", executed > 0);
    return executed;
}

int main() {
    const fs::path root = CONFORMANCE_CORPUS_DIR;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        check(root.generic_string() + ": is a directory", false);
        if (ec) std::printf("  %s\n", ec.message().c_str());
        return 1;
    }

    const std::vector<fs::path> case_dirs = find_case_dirs(root);
    if (case_dirs.empty()) {
        check(root.generic_string() + ": has at least one valid case dir", false);
        return 1;
    }
    int total_cases = 0;
    for (const auto& case_dir : case_dirs) {
        total_cases += run_case_dir(case_dir, root);
    }
    check("corpus has at least one case", total_cases > 0);
    std::printf("\n%d case(s) executed across %zu case dir(s)\n", total_cases,
                case_dirs.size());

    std::printf("\n%s\n", g_failures == 0 ? "ALL CONFORMANCE CASES PASSED"
                                          : "CONFORMANCE CASES FAILED");
    return g_failures == 0 ? 0 : 1;
}
