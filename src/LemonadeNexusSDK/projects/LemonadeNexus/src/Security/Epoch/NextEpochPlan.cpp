#include <LemonadeNexus/Security/Epoch/NextEpochPlan.hpp>

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

namespace nexus::security {

namespace {

inline constexpr std::string_view kPlanDomain = "lemonade-nexus/next-epoch-plan:v1";
inline constexpr std::string_view kReadinessDomain = "lemonade-nexus/candidate-readiness:v1";
inline constexpr std::string_view kHandoffDomain = "lemonade-nexus/epoch-handoff:v3";
inline constexpr std::string_view kVoteKeySetDomain = "lemonade-nexus/vote-key-set:v1";

}  // namespace

Digest next_epoch_plan_digest(const NextEpochPlan& plan) {
    CanonicalEncoder encoder(kPlanDomain);
    encoder.add_bytes(plan.network_id);
    encoder.add_u64(plan.current_epoch.underlying());
    encoder.add_u64(plan.next_epoch.underlying());
    encoder.add_u32(plan.attempt);
    encoder.add_u64(plan.checkpoint_height.underlying());
    encoder.add_bytes(plan.checkpoint_state_root);
    encoder.add_bytes(plan.eligibility_commitment);
    encoder.add_bytes(plan.selection_seed);
    encoder.add_u64(plan.selected.size());
    for (const auto& node : plan.selected) {
        encoder.add_bytes(node.bytes);
        const auto incarnation = plan.incarnations.find(node);
        encoder.add_u64(incarnation != plan.incarnations.end() ? incarnation->second.underlying() : 0u);
    }
    encoder.add_u16(plan.security_ruleset);
    encoder.add_u16(plan.consensus_ruleset);
    encoder.add_u16(static_cast<uint16_t>(plan.profile_id));
    encoder.add_u16(plan.profile_ruleset);
    return encoder.digest();
}

Digest candidate_readiness_digest(const CandidateReadiness& readiness) {
    CanonicalEncoder encoder(kReadinessDomain);
    encoder.add_bytes(readiness.network_id);
    encoder.add_bytes(readiness.plan_digest);
    encoder.add_u64(readiness.next_epoch.underlying());
    encoder.add_u64(readiness.entries.size());
    for (const auto& entry : readiness.entries) {
        encoder.add_bytes(entry.node.bytes);
        encoder.add_u64(entry.incarnation.underlying());
        encoder.add_bytes(entry.evidence_digest);
        encoder.add_bytes(entry.vote_key);
    }
    return encoder.digest();
}

Digest epoch_handoff_digest(const EpochHandoff& handoff) {
    CanonicalEncoder encoder(kHandoffDomain);
    encoder.add_bytes(handoff.network_id);
    encoder.add_u64(handoff.from_epoch.underlying());
    encoder.add_u64(handoff.to_epoch.underlying());
    encoder.add_bytes(handoff.plan_digest);
    encoder.add_bytes(handoff.previous_anchor);
    encoder.add_u64(handoff.members.size());
    for (const auto& node : handoff.members) {
        encoder.add_bytes(node.bytes);
        const auto incarnation = handoff.incarnations.find(node);
        encoder.add_u64(incarnation != handoff.incarnations.end() ? incarnation->second.underlying() : 0u);
    }
    encoder.add_bytes(vote_key_set_digest(handoff.vote_keys));
    encoder.add_bytes(handoff.group_public_key);
    encoder.add_bytes(handoff.dkg_transcript_digest);
    encoder.add_u64(handoff.key_generation.underlying());
    encoder.add_bytes(handoff.attestation_root);
    encoder.add_u16(handoff.security_ruleset);
    encoder.add_u16(handoff.consensus_ruleset);
    return encoder.digest();
}

Digest vote_key_set_digest(const std::map<NodeId, crypto::Ed25519PublicKey>& vote_keys) {
    CanonicalEncoder encoder(kVoteKeySetDomain);
    encoder.add_u64(vote_keys.size());
    for (const auto& [node, key] : vote_keys) {
        encoder.add_bytes(node.bytes);
        encoder.add_bytes(key);
    }
    return encoder.digest();
}

}  // namespace nexus::security
