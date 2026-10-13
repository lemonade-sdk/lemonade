#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// Mints a bare id that is unique within `claimed`, following the rule extra
// model discovery has always used: the bare name first, then
// `<qualifier>-<base>`, then `-2`, `-3`. `prefix` builds the full id when
// consulting `reserved`, which is why reserved ids are matched prefixed while
// claimed names are matched bare. The loser of a collision is renamed and
// kept, never dropped.
std::string reserve_unique_name(std::set<std::string>& claimed,
                                const std::set<std::string>& reserved,
                                const std::string& prefix,
                                const std::string& base,
                                const std::string& qualifier);

// Bare listing name to canonical model id. Catalog precedence decides who owns
// a name: the first insertion wins and a later one is reported as Shadowed,
// which is the behavior the current cache relied on for extra models whose ids
// were already made unique.
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

    // Storage is case-sensitive, matching discovery. Case-insensitive lookup
    // exists only so the one-time legacy ref migration can find what a stale
    // reference meant; resolution must not use it.
    std::vector<std::string> names_ignoring_case(const std::string& bare_name) const;

private:
    std::map<std::string, std::string> by_bare_name_;
    std::set<std::string> canonical_ids_;
};

}  // namespace models
}  // namespace lemon
