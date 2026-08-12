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

// A semantic_similarity trace score is computed (dot product, sqrt, division), so
// its last bits can differ across CI's x86/ARM runners; it is compared within this
// margin. Other scores come straight from stubs and are compared exactly.
static constexpr double kScoreTolerance = 1e-12;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// A failed check with an indented detail line (an error message, usually).
static void fail(const std::string& name, const std::string& detail) {
    check(name, false);
    std::printf("  %s\n", detail.c_str());
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
    return (ec || rel.empty()) ? path.lexically_relative(root).generic_string() : rel.generic_string();
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

// Corpus layout is exactly routing/<version>/<case>/{policy.json,cases.jsonl}.
// Anything off that shape — stray files, a missing file, a nested subdir, an
// unreadable dir — is a hard failure, not silently skipped.
static std::vector<fs::path> find_case_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    // Files directly under the root are docs (README.md), not corpus content.
    const DirEntries root_entries = list_entries(root, ec);
    if (ec) {
        fail(root.generic_string() + ": is readable", ec.message());
        return dirs;
    }
    for (const auto& version : root_entries.dirs) {
        std::error_code vec;
        const DirEntries version_entries = list_entries(version, vec);
        if (vec) {
            fail(rel_label(version, root) + ": is readable", vec.message());
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
                fail(rel + ": entries are readable", (policy_ec ? policy_ec : cases_ec).message());
                ok = false;
            } else if (!has_policy || !has_cases) {
                const std::string missing = (!has_policy && !has_cases)
                                                ? "policy.json and cases.jsonl"
                                                : (!has_policy ? "policy.json" : "cases.jsonl");
                fail(rel + ": has policy.json + cases.jsonl", "missing " + missing);
                ok = false;
            }
            if (sec) {
                fail(rel + ": is readable", sec.message());
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
                        fake.set_embed_failure(model.key(), text.key());
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

// The one place the score-tolerance rule lives: a semantic_similarity score may
// drift within kScoreTolerance; any other score (or a non-numeric one) is exact.
static bool score_matches(const json& expected_score, const json& produced_score, bool tolerant) {
    if (!tolerant || !expected_score.is_number() || !produced_score.is_number()) {
        return expected_score == produced_score;
    }
    return std::fabs(expected_score.get<double>() - produced_score.get<double>()) <= kScoreTolerance;
}

// True once `key` is set aside from both: the rest is identical and `key` is
// present in both or neither. The two match functions handle `key` themselves.
static bool equal_ignoring(const json& expected, const json& produced, const char* key) {
    json expected_rest = expected;
    json produced_rest = produced;
    expected_rest.erase(key);
    produced_rest.erase(key);
    return expected_rest == produced_rest && expected.contains(key) == produced.contains(key);
}

// `semantic_conditions` holds the trace condition strings ("classifier:<id>")
// whose score is computed by semantic_similarity and so is compared within
// kScoreTolerance. Every other score is compared exactly.
static bool trace_entries_match(const json& expected, const json& produced,
                                const std::set<std::string>& semantic_conditions) {
    if (!expected.is_object() || !produced.is_object()) return expected == produced;
    if (!equal_ignoring(expected, produced, "score")) return false;
    if (!expected.contains("score")) return true;
    const bool tolerant = semantic_conditions.count(expected.value("condition", "")) != 0;
    return score_matches(expected["score"], produced["score"], tolerant);
}

static bool decisions_match(const json& expected, const json& produced,
                            const std::set<std::string>& semantic_conditions) {
    if (!equal_ignoring(expected, produced, "trace")) return false;
    if (!expected.contains("trace")) return true;
    const json& expected_trace = expected["trace"];
    const json& produced_trace = produced["trace"];
    if (!expected_trace.is_array() || !produced_trace.is_array() ||
        expected_trace.size() != produced_trace.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected_trace.size(); ++i) {
        if (!trace_entries_match(expected_trace[i], produced_trace[i], semantic_conditions)) return false;
    }
    return true;
}

// True for a trace score delta decisions_match() already accepted within
// kScoreTolerance. json::diff is exact, so report_mismatch drops these to keep the
// report on the real difference; non-semantic scores are exact and never dropped.
static bool is_within_tolerance_score(const std::string& path, const json& expected,
                                      const json& produced,
                                      const std::set<std::string>& semantic_conditions) {
    static const std::string kSuffix = "/score";
    if (path.size() < kSuffix.size() ||
        path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return false;
    }
    const auto cond_ptr = json::json_pointer(path.substr(0, path.size() - kSuffix.size()) + "/condition");
    const auto score_ptr = json::json_pointer(path);
    if (!expected.contains(cond_ptr) || !expected.at(cond_ptr).is_string() ||
        !expected.contains(score_ptr) || !produced.contains(score_ptr)) {
        return false;
    }
    const bool tolerant = semantic_conditions.count(expected.at(cond_ptr).get<std::string>()) != 0;
    return tolerant && score_matches(expected.at(score_ptr), produced.at(score_ptr), tolerant);
}

static void report_mismatch(const json& expected, const json& produced,
                            const std::set<std::string>& semantic_conditions) {
    std::printf("  expected: %s\n", expected.dump().c_str());
    std::printf("  produced: %s\n", produced.dump().c_str());

    std::vector<json> diffs;
    for (const auto& op : json::diff(expected, produced)) {
        if (!is_within_tolerance_score(op.value("path", ""), expected, produced, semantic_conditions)) {
            diffs.push_back(op);
        }
    }

    std::printf("  %zu field(s) differ:\n", diffs.size());
    for (const auto& op : diffs) {
        const std::string path = op.value("path", "");
        const auto ptr = json::json_pointer(path);
        const std::string exp = expected.contains(ptr) ? expected.at(ptr).dump() : "<absent>";
        const std::string prod = produced.contains(ptr) ? produced.at(ptr).dump() : "<absent>";
        std::printf("    %s: expected: %s, produced: %s\n", path.c_str(), exp.c_str(), prod.c_str());
    }
}

// The directory name is the schema major the policy must declare, so a policy
// under the wrong version cannot pass unnoticed. Read once per directory.
static std::optional<json> load_policy_json(const fs::path& case_dir, const std::string& rel) {
    try {
        json policy_json = load_json_file(case_dir / "policy.json");
        const std::string directory_version = case_dir.parent_path().filename().string();
        if (!policy_json.contains("version") || !policy_json["version"].is_string() ||
            policy_json["version"].get<std::string>() != directory_version) {
            check(rel + ": policy version matches schema-major directory", false);
            return std::nullopt;
        }
        return policy_json;
    } catch (const std::exception& e) {
        fail(rel + ": policy.json parses", e.what());
        return std::nullopt;
    }
}

// Built per case: a semantic_similarity classifier caches its embeddings on its
// own instance, so a shared policy would pin every case to the first's vectors.
static std::optional<RoutePolicy> build_policy(const json& policy_json, const std::string& rel) {
    try {
        return lemon::parse_route_policy_collection(policy_json);
    } catch (const std::exception& e) {
        fail(rel + ": route policy builds", e.what());
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
        fail(rel + ": policy engine compiles", e.what());
        return std::nullopt;
    }
}

// One case per non-blank line. A row must be an object carrying a request and a
// decision, hold no key outside the allowlist, and have a name unique within the
// file: the coverage matrix maps one behavior to one named case.
static std::optional<json> read_case_row(const std::string& line, const std::string& rel,
                                         int line_no, std::set<std::string>& seen_names) {
    static const std::set<std::string> kAllowedRowKeys = {"name", "note", "request", "decision", "services"};
    const std::string where = rel + ": cases.jsonl line " + std::to_string(line_no);

    json row;
    try {
        row = json::parse(line);
    } catch (const std::exception& e) {
        fail(where + " parses", e.what());
        return std::nullopt;
    }
    if (!row.is_object() || !row.contains("request") || !row.contains("decision") ||
        !row["request"].is_object() || !row["decision"].is_object()) {
        check(where + " has object request+decision", false);
        return std::nullopt;
    }
    bool keys_ok = true;
    for (auto it = row.begin(); it != row.end(); ++it) {
        if (kAllowedRowKeys.count(it.key()) == 0) {
            check(where + " unknown key '" + it.key() + "'", false);
            keys_ok = false;
        }
    }
    if (!keys_ok) {
        return std::nullopt;
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
                     const lemon::testing::FakeClassifierServices& fake, const json& row,
                     const std::string& name) {
    const Decision decision = engine.route(request_context, row.at("request").value("route_trace", false));

    const json produced = lemon::route_decision_to_json(decision);
    const json& expected = row.at("decision");

    // Collect the semantic_similarity classifiers' trace conditions
    // ("classifier:<id>"); only their scores get the kScoreTolerance margin.
    std::set<std::string> semantic_conditions;
    for (const auto& entry : engine.policy().classifiers) {
        if (entry.second && entry.second->type() == "semantic_similarity") {
            semantic_conditions.insert("classifier:" + entry.first);
        }
    }

    // A backend call the case did not stub means the decision rests on a
    // placeholder default, so it fails regardless of whether the fields matched.
    const std::vector<std::string>& unexpected = fake.unexpected_calls();
    const bool ok = decisions_match(expected, produced, semantic_conditions) && unexpected.empty();
    check(name, ok);
    if (!unexpected.empty()) {
        std::printf("  unstubbed backend call(s):\n");
        for (const auto& call : unexpected) std::printf("    %s\n", call.c_str());
    } else if (!ok) {
        report_mismatch(expected, produced, semantic_conditions);
    }
}

static bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

static int run_case_dir(const fs::path& case_dir, const fs::path& root) {
    const std::string rel = rel_label(case_dir, root);

    std::ifstream cases(case_dir / "cases.jsonl");
    if (!cases) {
        check(rel + ": cases.jsonl opens", false);
        return 0;
    }

    // Read the shared policy once, up front, so a bad policy fails here instead
    // of on whichever row runs first.
    const std::optional<json> policy_json = load_policy_json(case_dir, rel);
    if (!policy_json) return 0;

    // Build + compile it once too, so a structurally bad policy fails the whole
    // directory here rather than on whichever row runs first. Compile never calls
    // the services, so an empty fake is enough. The per-case build below stays.
    lemon::testing::FakeClassifierServices probe;
    std::optional<RoutePolicy> probe_policy = build_policy(*policy_json, rel);
    if (!probe_policy) return 0;
    if (!compile_engine(std::move(*probe_policy), probe.make(), rel)) return 0;

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
        const lemon::RouteContext request_context = lemon::build_route_context(request, request.value("model", ""));

        // Fresh fake, policy and engine per case; the fake outlives the engine.
        lemon::testing::FakeClassifierServices fake;
        if (row->contains("services") &&
            !apply_row_services(fake, row->at("services"), name + ".services")) {
            continue;
        }

        std::optional<RoutePolicy> policy = build_policy(*policy_json, rel);
        if (!policy) return executed;
        std::optional<RoutingPolicyEngine> engine = compile_engine(std::move(*policy), fake.make(), rel);
        if (!engine) return executed;

        run_case(*engine, request_context, fake, *row, name);
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
    std::printf("\n%d case(s) executed across %zu case dir(s)\n", total_cases, case_dirs.size());

    std::printf("\n%s\n", g_failures == 0 ? "ALL CONFORMANCE CASES PASSED" : "CONFORMANCE CASES FAILED");
    return g_failures == 0 ? 0 : 1;
}
