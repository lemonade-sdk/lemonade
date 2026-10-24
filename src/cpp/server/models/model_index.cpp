#include "lemon/models/model_index.h"

#include <algorithm>
#include <cctype>

namespace lemon {
namespace models {

namespace {

std::string lower_ascii(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

std::string reserve_unique_name(std::set<std::string>& claimed,
                                const std::set<std::string>& reserved,
                                const std::string& prefix,
                                const std::string& base,
                                const std::string& qualifier) {
    auto taken = [&](const std::string& name) {
        return claimed.count(name) > 0 || reserved.count(prefix + name) > 0;
    };

    std::string name = base;
    if (taken(name)) {
        const std::string qualified = qualifier + "-" + base;
        name = qualified;
        for (int n = 2; taken(name); ++n) {
            name = qualified + "-" + std::to_string(n);
        }
    }
    claimed.insert(name);
    return name;
}

ModelIndex::InsertResult ModelIndex::add(const std::string& bare_name,
                                         const std::string& canonical_id) {
    if (canonical_ids_.count(canonical_id)) {
        return InsertResult::DuplicateId;
    }

    auto existing = by_bare_name_.find(bare_name);
    if (existing != by_bare_name_.end()) {
        if (existing->second == canonical_id) return InsertResult::DuplicateId;
        return InsertResult::Shadowed;
    }

    by_bare_name_.emplace(bare_name, canonical_id);
    canonical_ids_.insert(canonical_id);
    return InsertResult::Inserted;
}

bool ModelIndex::contains(const std::string& bare_name) const {
    return by_bare_name_.count(bare_name) > 0;
}

std::string ModelIndex::canonical_for(const std::string& bare_name) const {
    auto it = by_bare_name_.find(bare_name);
    return it != by_bare_name_.end() ? it->second : std::string();
}

bool ModelIndex::owns(const std::string& canonical_id) const {
    return canonical_ids_.count(canonical_id) > 0;
}

std::vector<std::string> ModelIndex::bare_names() const {
    std::vector<std::string> names;
    names.reserve(by_bare_name_.size());
    for (const auto& [bare, canonical] : by_bare_name_) {
        names.push_back(bare);
    }
    return names;
}

std::vector<std::string> ModelIndex::names_ignoring_case(const std::string& bare_name) const {
    const std::string needle = lower_ascii(bare_name);
    std::vector<std::string> matches;
    for (const auto& [bare, canonical] : by_bare_name_) {
        if (lower_ascii(bare) == needle) matches.push_back(bare);
    }
    return matches;
}

}  // namespace models
}  // namespace lemon
