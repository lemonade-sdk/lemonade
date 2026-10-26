#pragma once

// The finalized objects that carry a node from selected to active.
//
// Three records, three finality points, one authority. The current epoch's
// HotStuff finalizes the plan (who is selected), the readiness set (who proved
// preparation), and the handoff (what activates). Each is a pure function of
// finalized inputs, so every honest node derives the same record and votes for
// the same digest; a candidate outside the epoch verifies the commit proof
// instead of trusting whoever delivered it.
//
// None of these records grants current authority. The plan authorizes
// preparation; the readiness set authorizes a DKG; only the finalized handoff
// activates anything, and it activates the NEXT epoch.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Attestation/AttestationProfileId.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace nexus::security {

/// The canonical next-epoch plan. Derived deterministically from the finalized
/// eligibility state and the current epoch's selection seed; finalized as a
/// transitions digest in current-epoch consensus.
struct NextEpochPlan {
    NetworkId network_id{};
    EpochId current_epoch{};
    EpochId next_epoch{};

    /// The DKG attempt this plan authorizes. A failed candidate produces a new
    /// plan with the next attempt, never a resumed session.
    uint32_t attempt{};

    /// The finalized state the boundary is anchored to: the commit that
    /// finalized the eligibility state. Identical on every honest node.
    Height checkpoint_height{};
    Digest checkpoint_state_root{};
    Digest eligibility_commitment{};

    /// The selection inputs, so the selected list is checkable rather than
    /// merely stated.
    crypto::Ed25519PublicKey selection_seed{};

    /// Hash-rank order. Order matters: replacement pulls the next entry.
    std::vector<NodeId> selected;
    std::map<NodeId, IncarnationId> incarnations;

    SecurityRulesetVersion security_ruleset{};
    ConsensusRulesetVersion consensus_ruleset{};
    AttestationProfileId profile_id{AttestationProfileId::Unknown};
    AttestationProfileRuleset profile_ruleset{};
};

[[nodiscard]] Digest next_epoch_plan_digest(const NextEpochPlan& plan);

/// One selected node's proved preparation, as every current member derives it.
struct ReadinessEntry {
    NodeId node{};
    IncarnationId incarnation{};
    /// The plan-bound final-attestation context the node's evidence answered
    /// (SecurityDriver::plan_attest_context). Every member verified its own
    /// fresh evidence for this context; the evidence digests differ per
    /// verifier because each challenge carries its own nonce, so they cannot
    /// be part of a record that every member must derive identically.
    Digest attestation_context{};
    /// The next-epoch BFT vote key the evidence bound.
    crypto::Ed25519PublicKey vote_key{};

    auto operator<=>(const ReadinessEntry&) const = default;
};

/// The finalized readiness set: every selected member, attested and keyed,
/// under one plan. DKG for the plan's epoch starts only after this commits.
struct CandidateReadiness {
    NetworkId network_id{};
    Digest plan_digest{};
    EpochId next_epoch{};
    /// Entries sorted by node, so the digest is order-independent of arrival.
    std::vector<ReadinessEntry> entries;
};

[[nodiscard]] Digest candidate_readiness_digest(const CandidateReadiness& readiness);

/// The finalized handoff: everything the next epoch is, bound into the digest
/// the current epoch commits. Activation verifies this and nothing weaker.
struct EpochHandoff {
    NetworkId network_id{};
    EpochId from_epoch{};
    EpochId to_epoch{};
    Digest plan_digest{};

    /// The from-epoch's own authority anchor: the bootstrap certificate's
    /// signing digest for epoch 1, the previous finalized handoff's digest
    /// after that. Chains every handoff to its predecessor, so a verifier
    /// walking from Genesis rejects a gap, a fork, or a reordering by
    /// construction.
    Digest previous_anchor{};

    /// The new epoch, in full: membership, incarnations, and BFT vote keys.
    std::vector<NodeId> members;
    std::map<NodeId, IncarnationId> incarnations;
    std::map<NodeId, crypto::Ed25519PublicKey> vote_keys;

    crypto::Ed25519PublicKey group_public_key{};
    Digest dkg_transcript_digest{};
    KeyGeneration key_generation{};

    /// Commits to each new member and the plan-bound attestation context its
    /// readiness rests on (EpochManager::computed_attestation_root).
    Digest attestation_root{};
    SecurityRulesetVersion security_ruleset{};
    ConsensusRulesetVersion consensus_ruleset{};
};

[[nodiscard]] Digest epoch_handoff_digest(const EpochHandoff& handoff);

/// The vote-key set as one digest: count, then each (node, key) in node order.
/// The bootstrap certificate and every handoff commit to this, so a candidate
/// can verify a supplied key listing against finalized state.
[[nodiscard]] Digest vote_key_set_digest(
    const std::map<NodeId, crypto::Ed25519PublicKey>& vote_keys);

}  // namespace nexus::security
