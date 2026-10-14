#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// Bare name, then `<qualifier>-<base>`, then -2, -3; the loser is renamed.
std::string reserve_unique_name(std::set<std::string>& claimed,
                                const std::set<std::string>& reserved,
                                const std::string& prefix,
                                const std::string& base,
                                const std::string& qualifier);

// First insertion wins; a later one is Shadowed.
class ModelIndex {
public:
    enum class InsertResult {
        Inserted,
        DuplicateId,
        Shadowed,
    };

    InsertResult add(const std::string& bare_name, const std::string& canonical_id);

    bool contains(const std::string& bare_name) const;
    std::string canonical_for(const std::string& bare_name) const;
    bool owns(const std::string& canonical_id) const;
    size_t size() const { return by_bare_name_.size(); }
    std::vector<std::string> bare_names() const;

// Case-insensitive lookup exists only for the one-time legacy ref migration.
    std::vector<std::string> names_ignoring_case(const std::string& bare_name) const;

private:
    std::map<std::string, std::string> by_bare_name_;
    std::set<std::string> canonical_ids_;
};

}  // namespace models
}  // namespace lemon
