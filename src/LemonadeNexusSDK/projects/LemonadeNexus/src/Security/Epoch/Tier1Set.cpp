#include <LemonadeNexus/Security/Epoch/Tier1Set.hpp>

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace nexus::security {

namespace {

constexpr std::string_view kParticipantSetDomain = "lemonade-nexus/participant-set:v1";

}  // namespace

Tier1Set::Tier1Set(std::vector<NodeId> sorted_members) : members_(std::move(sorted_members)) {}

std::optional<Tier1Set> Tier1Set::from_nodes(std::vector<NodeId> nodes) {
    std::sort(nodes.begin(), nodes.end());
    if (std::adjacent_find(nodes.begin(), nodes.end()) != nodes.end()) {
        return std::nullopt;
    }
    return Tier1Set(std::move(nodes));
}

bool Tier1Set::contains(const NodeId& node) const {
    return std::binary_search(members_.begin(), members_.end(), node);
}

Digest Tier1Set::digest() const {
    CanonicalEncoder encoder(kParticipantSetDomain);
    encoder.add_u64(static_cast<uint64_t>(members_.size()));
    for (const auto& node : members_) {
        encoder.add_bytes(node.bytes);
    }
    return encoder.digest();
}

}  // namespace nexus::security
