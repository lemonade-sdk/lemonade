#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
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

// nlohmann keeps only the last of two identically-named keys, so a file that
// declares the same key twice inside one object parses cleanly and the first value
// is silently lost — a repeated policy name, default_model, rules or decision would
// pass unnoticed. Takes the raw file text and returns the dotted path of every key
// that repeats inside its own object, at any depth, in the order the repeats
// appear. Array elements are not indexed, so a key repeated inside a rule of a
// "rules" array reads "rules.decision".
inline std::vector<std::string> duplicate_object_keys(const std::string& text) {
    struct Scope {
        std::string label;
        std::set<std::string> keys;
    };
    std::vector<Scope> scopes;
    std::vector<std::string> duplicates;
    std::string pending_key;
    nlohmann::json::parser_callback_t collect =
        [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                case nlohmann::json::parse_event_t::array_start:
                    scopes.push_back({pending_key, {}});
                    pending_key.clear();
                    break;
                case nlohmann::json::parse_event_t::object_end:
                case nlohmann::json::parse_event_t::array_end:
                    if (!scopes.empty()) scopes.pop_back();
                    break;
                case nlohmann::json::parse_event_t::key: {
                    const std::string name = parsed.get<std::string>();
                    if (!scopes.empty() && !scopes.back().keys.insert(name).second) {
                        std::string path;
                        for (const auto& scope : scopes) {
                            if (!scope.label.empty()) path += scope.label + ".";
                        }
                        path += name;
                        if (std::find(duplicates.begin(), duplicates.end(), path) ==
                            duplicates.end()) {
                            duplicates.push_back(path);
                        }
                    }
                    pending_key = name;
                    break;
                }
                default:
                    break;
            }
            return true;
        };
    try {
        static_cast<void>(nlohmann::json::parse(text, collect));
    } catch (const std::exception&) {
        return {};
    }
    return duplicates;
}

enum class NameStatus { kOk, kMissing, kNotString, kDuplicate, kInvalidChars };

// A case label joins tier path, policy name and case name with "::", so a name
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

} // namespace conformance
} // namespace lemon
