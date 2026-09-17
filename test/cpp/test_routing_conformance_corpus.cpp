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

#include "test_conformance_row_harness.h"
#include "fake_classifier_services.h"
#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <algorithm>
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

// A leaf band directory must hold exactly policies.json + cases.jsonl and no
// subdirectory. Returns true only when it is well-formed; every deviation is a
// hard failure, not a silent skip.
static bool is_valid_band_dir(const fs::path& band_dir, const fs::path& root) {
    const std::string rel = rel_label(band_dir, root);

    std::error_code policies_ec;
    std::error_code cases_ec;
    const bool has_policies = fs::exists(band_dir / "policies.json", policies_ec);
    const bool has_cases = fs::exists(band_dir / "cases.jsonl", cases_ec);
    std::error_code sec;
    const DirEntries nested = list_entries(band_dir, sec);

    bool ok = true;
    if (policies_ec || cases_ec) {
        fail(rel + ": entries are readable", (policies_ec ? policies_ec : cases_ec).message());
        ok = false;
    } else if (!has_policies || !has_cases) {
        const std::string missing = (!has_policies && !has_cases)
                                        ? "policies.json and cases.jsonl"
                                        : (!has_policies ? "policies.json" : "cases.jsonl");
        fail(rel + ": has policies.json + cases.jsonl", "missing " + missing);
        ok = false;
    }
    if (sec) {
        fail(rel + ": is readable", sec.message());
        ok = false;
    } else {
        if (!nested.dirs.empty()) {
            check(rel + ": is a leaf (no subdirectories)", false);
            ok = false;
        }
        for (const auto& entry : nested.non_dirs) {
            const std::string fname = entry.filename().string();
            if (fname != "policies.json" && fname != "cases.jsonl") {
                check(rel_label(entry, root) + ": is policies.json or cases.jsonl", false);
                ok = false;
            }
        }
    }
    return ok;
}

// Corpus layout is exactly routing/<version>/<band>/{policies.json,cases.jsonl}.
// <band> groups cases by the engine tier they lock (l0a, l1, l2, l3). Anything off
// that shape — stray files, a missing file, an extra nesting level, an unreadable
// dir — is a hard failure, not silently skipped.
static std::vector<fs::path> find_band_dirs(const fs::path& root) {
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
            check(rel_label(stray, root) + ": is a band directory", false);
        }
        for (const auto& band_dir : version_entries.dirs) {
            if (is_valid_band_dir(band_dir, root)) {
                dirs.push_back(band_dir);
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
        if (lemon::conformance::allowed_service_names().count(name) == 0) {
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

static void report_mismatch(const json& expected, const json& produced,
                            const std::vector<std::string>& mismatches) {
    std::printf("  expected: %s\n", expected.dump().c_str());
    std::printf("  produced: %s\n", produced.dump().c_str());
    std::printf("  %zu field(s) differ:\n", mismatches.size());
    for (const auto& mismatch : mismatches) {
        std::printf("    %s\n", mismatch.c_str());
    }
}

// A band's policies.json is a name -> policy map; a case selects one by its
// policy_name. The version directory name (the band's parent) is the schema major
// every policy must declare, so a policy under the wrong version cannot pass
// unnoticed. Read once per band.
static std::optional<json> load_policies_json(const fs::path& band_dir, const std::string& rel) {
    json policies_json;
    try {
        policies_json = load_json_file(band_dir / "policies.json");
    } catch (const std::exception& e) {
        fail(rel + ": policies.json parses", e.what());
        return std::nullopt;
    }
    if (!policies_json.is_object() || policies_json.empty()) {
        check(rel + ": policies.json is a non-empty name -> policy map", false);
        return std::nullopt;
    }
    const std::string directory_version = band_dir.parent_path().filename().string();
    bool ok = true;
    for (auto it = policies_json.begin(); it != policies_json.end(); ++it) {
        const json& policy = it.value();
        if (!policy.is_object() || !policy.contains("version") || !policy["version"].is_string() ||
            policy["version"].get<std::string>() != directory_version) {
            check(rel + "/" + it.key() + ": policy version matches schema-major directory", false);
            ok = false;
        }
    }
    return ok ? std::optional<json>(std::move(policies_json)) : std::nullopt;
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
// decision, name a policy_name, hold no key outside the allowlist, and have a
// case_name unique within its policy: the coverage matrix maps one behavior to one
// named case, and (policy_name, case_name) is that case's identity. `seen_by_policy`
// tracks the case_names already accepted under each policy_name.
static std::optional<json> read_case_row(const std::string& line, const std::string& rel, int line_no,
                                         std::map<std::string, std::set<std::string>>& seen_by_policy) {
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
    const std::vector<std::string> unknown_keys = lemon::conformance::unknown_row_keys(row);
    for (const auto& key : unknown_keys) {
        check(where + " unknown key '" + key + "'", false);
    }
    if (!unknown_keys.empty()) {
        return std::nullopt;
    }
    const auto policy_it = row.find("policy_name");
    if (policy_it == row.end() || !policy_it->is_string() || policy_it->get<std::string>().empty()) {
        check(where + " has a policy_name", false);
        return std::nullopt;
    }
    const std::string policy_name = policy_it->get<std::string>();
    if (!lemon::conformance::name_chars_ok(policy_name)) {
        check(where + " policy_name '" + policy_name + "' has no separator or whitespace", false);
        return std::nullopt;
    }
    switch (lemon::conformance::check_case_name(row, seen_by_policy[policy_name])) {
        case lemon::conformance::NameStatus::kMissing:
            check(where + " has a case_name", false);
            return std::nullopt;
        case lemon::conformance::NameStatus::kNotString:
            check(where + " case_name is a string", false);
            return std::nullopt;
        case lemon::conformance::NameStatus::kDuplicate:
            check(where + " duplicate case name '" + policy_name + "::" + row.value("case_name", "") + "'", false);
            return std::nullopt;
        case lemon::conformance::NameStatus::kInvalidChars:
            check(where + " case_name '" + row.value("case_name", "") +
                      "' has no separator or whitespace",
                  false);
            return std::nullopt;
        case lemon::conformance::NameStatus::kOk:
            break;
    }
    // check_case_name accepted it, so "case_name" is a non-empty string here.
    seen_by_policy[policy_name].insert(row.value("case_name", ""));
    return row;
}

static void run_case(const RoutingPolicyEngine& engine, const lemon::RouteContext& request_context,
                     const lemon::testing::FakeClassifierServices& fake, const json& row,
                     const std::string& name) {
    const Decision decision = engine.route(request_context, row.at("request").value("route_trace", false));

    const json produced = lemon::route_decision_to_json(decision);
    const json& expected = row.at("decision");

    const std::vector<std::string> mismatches =
        lemon::conformance::compare_decision(expected, produced);

    // A backend call the case did not stub means the decision rests on a
    // placeholder default, so it fails regardless of whether the fields matched.
    const std::vector<std::string>& unexpected = fake.unexpected_calls();
    const bool ok = mismatches.empty() && unexpected.empty();
    check(name, ok);
    if (!unexpected.empty()) {
        std::printf("  unstubbed backend call(s):\n");
        for (const auto& call : unexpected) std::printf("    %s\n", call.c_str());
    } else if (!ok) {
        report_mismatch(expected, produced, mismatches);
    }
}

static bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

static int run_band_dir(const fs::path& band_dir, const fs::path& root) {
    const std::string rel = rel_label(band_dir, root);

    // Read the band's policies once, up front, so a bad policy fails here instead
    // of on whichever case uses it first.
    const std::optional<json> policies_json = load_policies_json(band_dir, rel);
    if (!policies_json) return 0;

    // Build + compile each policy once too, so a structurally bad policy fails the
    // whole band here rather than on whichever case uses it first. Compile never
    // calls the services, so an empty fake is enough. The per-case build below stays.
    for (auto it = policies_json->begin(); it != policies_json->end(); ++it) {
        const std::string prel = rel + "/" + it.key();
        lemon::testing::FakeClassifierServices probe;
        std::optional<RoutePolicy> probe_policy = build_policy(it.value(), prel);
        if (!probe_policy) return 0;
        if (!compile_engine(std::move(*probe_policy), probe.make(), prel)) return 0;
    }

    std::ifstream cases(band_dir / "cases.jsonl");
    if (!cases) {
        check(rel + ": cases.jsonl opens", false);
        return 0;
    }

    int executed = 0;
    int line_no = 0;
    std::string line;
    std::map<std::string, std::set<std::string>> seen_by_policy;
    std::set<std::string> used_policies;
    while (std::getline(cases, line)) {
        ++line_no;
        if (is_blank(line)) continue;

        std::optional<json> row = read_case_row(line, rel, line_no, seen_by_policy);
        if (!row) continue;

        const std::string policy_name = row->at("policy_name").get<std::string>();
        // "::" marks where the band's real directory path stops; the two names after
        // it are JSON field values, not directories.
        const std::string name = rel + "::" + policy_name + "::" + row->at("case_name").get<std::string>();

        const auto policy_entry = policies_json->find(policy_name);
        if (policy_entry == policies_json->end()) {
            check(name + ": policy_name is defined in policies.json", false);
            continue;
        }
        used_policies.insert(policy_name);

        const json& request = row->at("request");
        const lemon::RouteContext request_context = lemon::build_route_context(request, request.value("model", ""));

        // Fresh fake, policy and engine per case; the fake outlives the engine.
        lemon::testing::FakeClassifierServices fake;
        if (row->contains("services") && !apply_row_services(fake, row->at("services"), name + ".services")) {
            continue;
        }

        std::optional<RoutePolicy> policy = build_policy(policy_entry.value(), name);
        if (!policy) return executed;
        std::optional<RoutingPolicyEngine> engine = compile_engine(std::move(*policy), fake.make(), name);
        if (!engine) return executed;

        run_case(*engine, request_context, fake, *row, name);
        ++executed;
    }

    check(rel + ": cases.jsonl has at least one case", executed > 0);
    // Every declared policy must be exercised by at least one case; an unused
    // policy is dead weight the corpus should not carry silently.
    for (auto it = policies_json->begin(); it != policies_json->end(); ++it) {
        check(rel + "::" + it.key() + ": policy is used by at least one case", used_policies.count(it.key()) != 0);
    }
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

    const std::vector<fs::path> band_dirs = find_band_dirs(root);
    if (band_dirs.empty()) {
        check(root.generic_string() + ": has at least one valid band dir", false);
        return 1;
    }
    int total_cases = 0;
    for (const auto& band_dir : band_dirs) {
        total_cases += run_band_dir(band_dir, root);
    }
    check("corpus has at least one case", total_cases > 0);
    std::printf("\n%d case(s) executed across %zu band dir(s)\n", total_cases, band_dirs.size());

    std::printf("\n%s\n", g_failures == 0 ? "ALL CONFORMANCE CASES PASSED" : "CONFORMANCE CASES FAILED");
    return g_failures == 0 ? 0 : 1;
}
