#pragma once

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Pure row-level guards for the conformance corpus runner. They hold no state and
// touch no globals, so the runner uses them to validate each cases.jsonl row and a
// unit test can exercise them in memory. The filesystem-walking guards (symlink
// loops, nested dirs, missing files) stay in the runner: they are expensive to
// fixture and unlikely to regress silently.

namespace lemon {
namespace conformance {

// The keys a cases.jsonl row may carry. A row with any other key is rejected so a
// typo'd field (e.g. "expected" for "decision") fails loudly instead of running
// with a silently-missing value.
inline const std::set<std::string>& allowed_row_keys() {
    static const std::set<std::string> keys = {"case_name", "policy_name", "note",
                                               "request",   "decision",    "services"};
    return keys;
}

// The service names a row's "services" object may declare.
inline const std::set<std::string>& allowed_service_names() {
    static const std::set<std::string> names = {"embed", "run_classifier", "chat"};
    return names;
}

// Keys present in `row` that are not in the allowlist, in iteration order.
inline std::vector<std::string> unknown_row_keys(const nlohmann::json& row) {
    std::vector<std::string> unknown;
    if (!row.is_object()) return unknown;
    for (auto it = row.begin(); it != row.end(); ++it) {
        if (allowed_row_keys().count(it.key()) == 0) unknown.push_back(it.key());
    }
    return unknown;
}

// Service names present in `services` that are not in the allowlist, in iteration
// order.
inline std::vector<std::string> unknown_service_names(const nlohmann::json& services) {
    std::vector<std::string> unknown;
    if (!services.is_object()) return unknown;
    for (auto it = services.begin(); it != services.end(); ++it) {
        if (allowed_service_names().count(it.key()) == 0) unknown.push_back(it.key());
    }
    return unknown;
}

enum class NameStatus { kOk, kMissing, kNotString, kDuplicate, kInvalidChars };

// A case label joins band path, policy name and case name with "::", so a name
// holding a separator or whitespace would let two different rows render to the
// same label.
inline bool name_chars_ok(const std::string& name) {
    return name.find_first_of(" \t\r\n/:") == std::string::npos;
}

// A row's "case_name": missing/empty, present but not a string, holding a
// separator, a duplicate of one already accepted, or ok. Pure — does not mutate
// `seen_names`; the caller records the name after accepting it.
inline NameStatus check_case_name(const nlohmann::json& row,
                                  const std::set<std::string>& seen_names) {
    const auto it = row.is_object() ? row.find("case_name") : row.end();
    if (it == row.end()) return NameStatus::kMissing;
    if (!it->is_string()) return NameStatus::kNotString;
    const std::string name = it->get<std::string>();
    if (name.empty()) return NameStatus::kMissing;
    if (!name_chars_ok(name)) return NameStatus::kInvalidChars;
    if (seen_names.count(name) != 0) return NameStatus::kDuplicate;
    return NameStatus::kOk;
}

// A semantic_similarity score is computed (dot product, square root, division), so
// its last bits can differ across CI's x86/ARM runners.
inline constexpr double kScoreTolerance = 1e-12;

namespace detail {

inline std::string field_path(const std::string& prefix, const std::string& name) {
    return prefix.empty() ? name : prefix + "." + name;
}

// Exact equality when `tolerance` is empty. Given one, two numbers may differ by up
// to that much; non-numbers stay exact either way, so a malformed score cannot slip
// through the margin.
inline bool values_match(const nlohmann::json& expected, const nlohmann::json& produced,
                         const std::optional<double>& tolerance) {
    if (!tolerance || !expected.is_number() || !produced.is_number()) return expected == produced;
    return std::fabs(expected.get<double>() - produced.get<double>()) <= *tolerance;
}

// Reports a field carried by only one side. True when both sides carry it.
inline bool both_carry(const nlohmann::json& expected, const nlohmann::json& produced,
                       const char* name, const std::string& prefix,
                       std::vector<std::string>& out) {
    const bool in_expected = expected.contains(name);
    const bool in_produced = produced.contains(name);
    if (in_expected && in_produced) return true;
    if (in_expected != in_produced) {
        out.push_back(field_path(prefix, name) + ": present in the " +
                      (in_expected ? "expected" : "produced") + " decision only");
    }
    return false;
}

// Compares one field and reports it when it differs. `tolerance` is empty for all
// but a trace `score` whose condition is a semantic_similarity classifier; every
// other field is a string, a boolean or recorded JSON and must match exactly. Each
// field carries its own tolerance, so a second computed field would not disturb
// this one.
inline void compare_field(const nlohmann::json& expected, const nlohmann::json& produced,
                          const char* name, const std::string& prefix,
                          const std::optional<double>& tolerance,
                          std::vector<std::string>& out) {
    if (!both_carry(expected, produced, name, prefix, out)) return;
    if (!values_match(expected[name], produced[name], tolerance)) {
        out.push_back(field_path(prefix, name) + ": expected " + expected[name].dump() +
                      ", produced " + produced[name].dump());
    }
}

// The serializer always emits these, so a field absent from both sides means the
// case records less than a full decision and nothing would be compared.
inline void report_missing_fields(const nlohmann::json& expected, const nlohmann::json& produced,
                                  std::initializer_list<const char*> always_emitted,
                                  const std::string& prefix, std::vector<std::string>& out) {
    for (const char* name : always_emitted) {
        if (!expected.contains(name) && !produced.contains(name)) {
            out.push_back(field_path(prefix, name) + ": missing from both sides");
        }
    }
}

// A field neither side's comparison knows about is drift between the serializer and
// this file, so it fails the case rather than going unchecked.
inline void report_unexpected_fields(const nlohmann::json& expected,
                                     const nlohmann::json& produced,
                                     const std::set<std::string>& known,
                                     const std::string& prefix, std::vector<std::string>& out) {
    std::set<std::string> present;
    for (const nlohmann::json* side : {&expected, &produced}) {
        for (auto it = side->begin(); it != side->end(); ++it) present.insert(it.key());
    }
    for (const auto& name : present) {
        if (known.count(name) == 0) out.push_back(field_path(prefix, name) + ": unexpected field");
    }
}

inline void compare_trace_entry(const nlohmann::json& expected, const nlohmann::json& produced,
                                const std::string& path, std::vector<std::string>& out) {
    compare_field(expected, produced, "condition", path, std::nullopt, out);
    compare_field(expected, produced, "result", path, std::nullopt, out);
    compare_field(expected, produced, "score", path, kScoreTolerance, out);
    compare_field(expected, produced, "label", path, std::nullopt, out);
    compare_field(expected, produced, "rationale", path, std::nullopt, out);

    report_missing_fields(expected, produced, {"condition", "result"}, path, out);
    report_unexpected_fields(expected, produced,
                             {"condition", "result", "score", "label", "rationale"}, path, out);
}

inline void compare_trace(const nlohmann::json& expected, const nlohmann::json& produced,
                          std::vector<std::string>& out) {
    if (!both_carry(expected, produced, "trace", "", out)) return;

    const nlohmann::json& expected_trace = expected["trace"];
    const nlohmann::json& produced_trace = produced["trace"];
    if (!expected_trace.is_array() || !produced_trace.is_array()) {
        out.push_back("trace: expected a JSON array on both sides");
        return;
    }
    if (expected_trace.size() != produced_trace.size()) {
        out.push_back("trace: expected " + std::to_string(expected_trace.size()) +
                      " entries, produced " + std::to_string(produced_trace.size()));
        return;
    }
    for (std::size_t i = 0; i < expected_trace.size(); ++i) {
        const std::string path = "trace[" + std::to_string(i) + "]";
        if (!expected_trace[i].is_object() || !produced_trace[i].is_object()) {
            out.push_back(path + ": expected a JSON object on both sides");
            continue;
        }
        compare_trace_entry(expected_trace[i], produced_trace[i], path, out);
    }
}

} // namespace detail

// Every way `produced` differs from the recorded `expected` decision; empty when
// they match.
inline std::vector<std::string> compare_decision(const nlohmann::json& expected,
                                                 const nlohmann::json& produced) {
    std::vector<std::string> out;
    if (!expected.is_object() || !produced.is_object()) {
        out.push_back("decision: expected a JSON object on both sides");
        return out;
    }

    detail::compare_field(expected, produced, "version", "", std::nullopt, out);
    detail::compare_field(expected, produced, "route_to", "", std::nullopt, out);
    detail::compare_field(expected, produced, "matched_rule", "", std::nullopt, out);
    detail::compare_field(expected, produced, "default_used", "", std::nullopt, out);
    detail::compare_field(expected, produced, "outputs", "", std::nullopt, out);
    detail::compare_trace(expected, produced, out);

    detail::report_missing_fields(
        expected, produced, {"version", "route_to", "matched_rule", "default_used", "outputs"}, "",
        out);
    detail::report_unexpected_fields(
        expected, produced,
        {"version", "route_to", "matched_rule", "default_used", "outputs", "trace"}, "", out);
    return out;
}

} // namespace conformance
} // namespace lemon
