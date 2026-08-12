#pragma once

#include <nlohmann/json.hpp>
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
    static const std::set<std::string> keys = {"name", "note", "request", "decision", "services"};
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

enum class NameStatus { kOk, kMissing, kDuplicate };

// A row's "name": missing/empty/non-string, a duplicate of one already accepted,
// or ok. Pure — does not mutate `seen_names`; the caller records the name after
// accepting it.
inline NameStatus check_case_name(const nlohmann::json& row,
                                  const std::set<std::string>& seen_names) {
    const std::string name = row.value("name", "");
    if (name.empty()) return NameStatus::kMissing;
    if (seen_names.count(name) != 0) return NameStatus::kDuplicate;
    return NameStatus::kOk;
}

} // namespace conformance
} // namespace lemon
