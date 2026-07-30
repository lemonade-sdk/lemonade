// Back-compat conformance corpus runner (#2425).
//
// Replays every golden case under test/conformance/routing/ through the real
// routing engine and asserts the emitted Decision (via the production
// route_decision_to_json serializer) equals the recorded expectation: same
// fields, same values. Any drift is a back-compat violation.
//
// Deterministic cases need no backend. Model-backed cases bind the engine to
// FakeClassifierServices and declare the answers it returns, so a case tests
// the engine's threshold and selection logic rather than a real model's floats.

#include "fake_classifier_services.h"
#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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

// Every Decision field is compared exactly except a trace score, which is
// computed (dot product, square roots, a division) and so can differ in its last
// bits between CI's x86 and ARM runners. The margin is far above that noise and
// far below any score difference a case could care about.
static constexpr double kScoreTolerance = 1e-12;

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

static bool parse_vector(const json& value, std::vector<float>& out) {
    if (!value.is_array()) return false;
    out.clear();
    for (const auto& item : value) {
        if (!item.is_number()) return false;
        out.push_back(item.get<float>());
    }
    return true;
}

// A case's stub answers. Every embedding is keyed by the text it is returned
// for, including the routing input's own text. A null answer makes that service
// fail, so the classifier's on_error applies.
//
//   {"embed":          {"<model>": {"<text>": [numbers] | null}},
//    "run_classifier": {"<model>": {"<label>": number} | null},
//    "chat":           {"<model>": "<reply>" | null}}
static bool apply_row_services(lemon::testing::FakeClassifierServices& fake, const json& spec,
                               const std::string& where) {
    if (!spec.is_object()) {
        check(where + ": is an object", false);
        return false;
    }
    bool ok = true;
    for (auto service = spec.begin(); service != spec.end(); ++service) {
        const std::string& name = service.key();
        if (name != "embed" && name != "run_classifier" && name != "chat") {
            check(where + ": unknown service '" + name + "'", false);
            ok = false;
            continue;
        }
        if (!service.value().is_object()) {
            check(where + "." + name + ": is a model -> answer map", false);
            ok = false;
            continue;
        }
        for (auto model = service.value().begin(); model != service.value().end(); ++model) {
            const json& answer = model.value();
            const std::string label = where + "." + name + "." + model.key();
            if (name == "embed") {
                if (!answer.is_object()) {
                    check(label + ": is a text -> vector map", false);
                    ok = false;
                    continue;
                }
                for (auto text = answer.begin(); text != answer.end(); ++text) {
                    std::vector<float> vec;
                    if (text.value().is_null()) {
                        fake.set_embedding(model.key(), text.key(), std::vector<float>{});
                    } else if (parse_vector(text.value(), vec)) {
                        fake.set_embedding(model.key(), text.key(), std::move(vec));
                    } else {
                        check(label + "." + text.key() + ": is a number array or null", false);
                        ok = false;
                    }
                }
            } else if (name == "run_classifier") {
                if (answer.is_null()) {
                    fake.set_classifier_failure(model.key());
                    continue;
                }
                std::map<std::string, double> scores;
                bool scores_ok = answer.is_object();
                for (auto score = answer.begin(); scores_ok && score != answer.end(); ++score) {
                    if (!score.value().is_number()) {
                        scores_ok = false;
                        break;
                    }
                    scores[score.key()] = score.value().get<double>();
                }
                if (!scores_ok) {
                    check(label + ": is a label -> number map or null", false);
                    ok = false;
                    continue;
                }
                fake.set_classifier_scores(model.key(), std::move(scores));
            } else {
                if (answer.is_null()) {
                    fake.set_chat_failure(model.key());
                } else if (answer.is_string()) {
                    fake.set_chat_reply(model.key(), answer.get<std::string>());
                } else {
                    check(label + ": is a string or null", false);
                    ok = false;
                }
            }
        }
    }
    return ok;
}

static bool trace_entries_match(const json& expected, const json& produced) {
    if (!expected.is_object() || !produced.is_object()) return expected == produced;
    json expected_rest = expected;
    json produced_rest = produced;
    expected_rest.erase("score");
    produced_rest.erase("score");
    if (expected_rest != produced_rest) return false;
    if (expected.contains("score") != produced.contains("score")) return false;
    if (!expected.contains("score")) return true;
    if (!expected["score"].is_number() || !produced["score"].is_number()) {
        return expected["score"] == produced["score"];
    }
    return std::fabs(expected["score"].get<double>() - produced["score"].get<double>()) <=
           kScoreTolerance;
}

static bool decisions_match(const json& expected, const json& produced) {
    json expected_rest = expected;
    json produced_rest = produced;
    expected_rest.erase("trace");
    produced_rest.erase("trace");
    if (expected_rest != produced_rest) return false;
    if (expected.contains("trace") != produced.contains("trace")) return false;
    if (!expected.contains("trace")) return true;
    const json& expected_trace = expected["trace"];
    const json& produced_trace = produced["trace"];
    if (!expected_trace.is_array() || !produced_trace.is_array() ||
        expected_trace.size() != produced_trace.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected_trace.size(); ++i) {
        if (!trace_entries_match(expected_trace[i], produced_trace[i])) return false;
    }
    return true;
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
    // TODO: maybe delete the trace piece, think about it
    if (!expected.contains("trace") || !produced.contains("trace")) return;
    const json& expected_trace = expected["trace"];
    const json& produced_trace = produced["trace"];
    if (!expected_trace.is_array() || !produced_trace.is_array()) return;
    for (std::size_t i = 0; i < expected_trace.size() && i < produced_trace.size(); ++i) {
        const json& e = expected_trace[i];
        const json& p = produced_trace[i];
        if (!e.is_object() || !p.is_object()) continue;
        if (!e.contains("score") || !p.contains("score")) continue;
        if (!e["score"].is_number() || !p["score"].is_number()) continue;
        const double delta = std::fabs(e["score"].get<double>() - p["score"].get<double>());
        if (delta > kScoreTolerance) {
            std::printf("    /trace/%zu/score: |delta| %.3g exceeds tolerance %.3g\n", i, delta,
                        kScoreTolerance);
        }
    }
}

// The case dir name is the schema major the policy must declare, so a policy
// filed under the wrong version cannot pass unnoticed.
static std::optional<RoutePolicy> load_case_policy(const fs::path& case_dir,
                                                   const std::string& rel) {
    try {
        const json policy_json = load_json_file(case_dir / "policy.json");
        const std::string directory_version = case_dir.parent_path().filename().string();
        if (!policy_json.contains("version") || !policy_json["version"].is_string() ||
            policy_json["version"].get<std::string>() != directory_version) {
            check(rel + ": policy version matches schema-major directory", false);
            return std::nullopt;
        }
        return lemon::parse_route_policy_collection(policy_json);
    } catch (const std::exception& e) {
        check(rel + ": policy.json parses", false);
        std::printf("  %s\n", e.what());
        return std::nullopt;
    }
}

// A policy can parse and still fail to compile (bad nesting, unresolved
// classifier ref), so rule compilation gets its own guard.
static std::optional<RoutingPolicyEngine> compile_engine(RoutePolicy policy,
                                                         lemon::ClassifierServices services,
                                                         const std::string& rel) {
    try {
        return RoutingPolicyEngine(std::move(policy), std::move(services));
    } catch (const std::exception& e) {
        check(rel + ": policy engine compiles", false);
        std::printf("  %s\n", e.what());
        return std::nullopt;
    }
}

// One case per non-blank line. A row must be an object carrying a request and a
// decision, hold no key outside the allowlist, and have a name unique within the
// file: the coverage matrix maps one behavior to one named case.
static std::optional<json> read_case_row(const std::string& line, const std::string& rel,
                                         int line_no, std::set<std::string>& seen_names) {
    static const std::set<std::string> kAllowedRowKeys = {"name", "note", "request", "decision",
                                                          "services"};
    const std::string where = rel + ": cases.jsonl line " + std::to_string(line_no);

    json row;
    try {
        row = json::parse(line);
    } catch (const std::exception& e) {
        check(where + " parses", false);
        std::printf("  %s\n", e.what());
        return std::nullopt;
    }
    if (!row.is_object() || !row.contains("request") || !row.contains("decision") ||
        !row["request"].is_object() || !row["decision"].is_object()) {
        check(where + " has object request+decision", false);
        return std::nullopt;
    }
    for (auto it = row.begin(); it != row.end(); ++it) {
        if (kAllowedRowKeys.count(it.key()) == 0) {
            check(where + " unknown key '" + it.key() + "'", false);
        }
    }
    const std::string case_name = row.value("name", "");
    if (case_name.empty()) {
        check(where + " has a name", false);
        return std::nullopt;
    }
    if (!seen_names.insert(case_name).second) {
        check(where + " duplicate case name '" + case_name + "'", false);
        return std::nullopt;
    }
    return row;
}

static void run_case(const RoutingPolicyEngine& engine, const lemon::RouteContext& request_context,
                     const json& row, const std::string& name) {
    const Decision decision =
        engine.route(request_context, row.at("request").value("route_trace", false));

    const json produced = lemon::route_decision_to_json(decision);
    const json& expected = row.at("decision");
    const bool ok = decisions_match(expected, produced);
    check(name, ok);
    if (!ok) report_mismatch(expected, produced);
}

static bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

static int run_case_dir(const fs::path& case_dir, const fs::path& root) {
    const std::string rel = rel_label(case_dir, root);

    // Declared before the engine: make() binds services to this object, so it
    // has to outlive them.
    lemon::testing::FakeClassifierServices fake;

    std::optional<RoutePolicy> policy = load_case_policy(case_dir, rel);
    if (!policy) return 0;

    std::optional<RoutingPolicyEngine> engine =
        compile_engine(std::move(*policy), fake.make(), rel);
    if (!engine) return 0;

    std::ifstream cases(case_dir / "cases.jsonl");
    if (!cases) {
        check(rel + ": cases.jsonl opens", false);
        return 0;
    }

    int executed = 0;
    int line_no = 0;
    std::string line;
    std::set<std::string> seen_names;
    while (std::getline(cases, line)) {
        ++line_no;
        if (is_blank(line)) continue;

        std::optional<json> row = read_case_row(line, rel, line_no, seen_names);
        if (!row) continue;

        const std::string name = rel + "/" + row->at("name").get<std::string>();
        const json& request = row->at("request");
        const lemon::RouteContext request_context =
            lemon::build_route_context(request, request.value("model", ""));

        fake.reset();
        if (row->contains("services") &&
            !apply_row_services(fake, row->at("services"), name + ".services")) {
            continue;
        }

        run_case(*engine, request_context, *row, name);
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
