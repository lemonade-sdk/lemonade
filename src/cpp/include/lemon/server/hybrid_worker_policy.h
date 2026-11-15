#pragma once

#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Pure decision boundaries shared by streaming and non-streaming Hybrid Worker
// paths. They do not touch the visible transcript or the model's KV cache.
namespace lemon::hybrid_worker_policy {

enum class Outcome { External, Done, TextDone };

inline Outcome classify(const std::vector<std::string>& names, const std::string& done_name) {
    if (names.empty()) {
        throw std::runtime_error("Hybrid worker returned no tool calls");
    }
    bool has_done = false;
    for (const auto& name : names) {
        if (name.empty()) {
            throw std::runtime_error("Hybrid worker returned an unnamed tool call");
        }
        has_done |= name == done_name;
    }
    if (has_done && (names.size() != 1 || names.front() != done_name)) {
        throw std::runtime_error("Hybrid worker mixed its internal completion with external tools");
    }
    return has_done ? Outcome::Done : Outcome::External;
}

inline Outcome classify_reply(const std::vector<std::string>& names,
                              const std::string& done_name,
                              const std::string& content,
                              const std::string& finish_reason) {
    if (!names.empty()) return classify(names, done_name);
    bool has_text = false;
    for (unsigned char c : content) {
        if (!std::isspace(c)) { has_text = true; break; }
    }
    if (finish_reason != "stop" || !has_text) {
        throw std::runtime_error("Hybrid worker did not produce a complete tool call or final report");
    }
    return Outcome::TextDone;
}

inline void validate_external(const std::vector<std::string>& names,
                              const std::vector<std::string>& ids,
                              const std::set<std::string>& declared_names) {
    if (names.empty() || names.size() != ids.size()) {
        throw std::runtime_error("Hybrid external tool-call list is incomplete");
    }
    std::set<std::string> used_ids;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!declared_names.count(names[i])) {
            throw std::runtime_error("Hybrid worker used an undeclared client tool");
        }
        if (ids[i].empty() || !used_ids.insert(ids[i]).second) {
            throw std::runtime_error("Hybrid worker emitted an empty or duplicate tool ID");
        }
    }
}

inline bool possible_name_prefix(const std::string& partial,
                                 const std::set<std::string>& external_names,
                                 const std::string& done_name) {
    if (done_name.compare(0, partial.size(), partial) == 0) return true;
    for (const auto& name : external_names) {
        if (name.compare(0, partial.size(), partial) == 0) return true;
    }
    return false;
}

// A thinking-only or blank reply, especially with finish_reason=length, must
// not be treated as a successful final assistant turn by Pi or other clients.
inline bool unusable_visible_reply(bool has_text, bool has_tool_calls) {
    return !has_text && !has_tool_calls;
}

} // namespace lemon::hybrid_worker_policy
