#pragma once

// The frozen Tier 1 participant set for one epoch.
//
// The set is immutable, sorted, and unique. Its digest is the
// participant_set_digest that DKG messages and epoch transitions bind to, so
// every correct node must derive the same digest from the same membership.

#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace nexus::security {

class Tier1Set {
public:
    /// Sorts the nodes and rejects a duplicate member. A duplicate is a
    /// protocol violation, not something to repair silently.
    [[nodiscard]] static std::optional<Tier1Set> from_nodes(std::vector<NodeId> nodes);

    [[nodiscard]] const std::vector<NodeId>& members() const { return members_; }
    [[nodiscard]] std::size_t size() const { return members_.size(); }
    [[nodiscard]] bool contains(const NodeId& node) const;

    /// The participant_set_digest: member count, then each member in sorted
    /// order, under the participant-set domain.
    [[nodiscard]] Digest digest() const;

private:
    explicit Tier1Set(std::vector<NodeId> sorted_members);

    std::vector<NodeId> members_;
};

}  // namespace nexus::security
