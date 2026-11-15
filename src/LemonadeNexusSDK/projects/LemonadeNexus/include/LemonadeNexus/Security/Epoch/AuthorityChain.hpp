#pragma once

// The verified epoch authority chain.
//
// One continuous line of trust: the pinned Genesis certificate authorizes
// Epoch 1, and each epoch's finalized handoff — proved under that epoch's own
// frozen vote keys — authorizes exactly the next one. A node holds one
// VerifiedEpochAuthority at a time and advances it link by link; no epoch
// ever appears from an announcement, a peer's word, or local configuration.
//
// Verification is pure derivation over supplied bytes. Peers supply candidate
// chain data; they never supply trust.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Consensus/CommitProof.hpp>
#include <LemonadeNexus/Security/Epoch/NextEpochPlan.hpp>
#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace nexus::security {

/// The locally verified authority state for one epoch: who Tier 1 is, under
/// which keys and rules, and how this epoch chains to its predecessor. A
/// record exists only as the output of verification — from the Genesis
/// certificate for Epoch 1, from a verified finalized handoff after that.
struct VerifiedEpochAuthority {
    NetworkId network_id{};
    EpochId epoch{};

    /// The epoch's full membership, in Tier1Set order, with the incarnation
    /// and BFT vote key the finalized record bound for each member.
    std::vector<NodeId> members;
    std::map<NodeId, IncarnationId> incarnations;
    std::map<NodeId, crypto::Ed25519PublicKey> vote_keys;

    std::size_t consensus_quorum{};
    std::size_t authority_threshold{};

    SecurityRulesetVersion security_ruleset{};
    ConsensusRulesetVersion consensus_ruleset{};

    crypto::Ed25519PublicKey group_public_key{};
    KeyGeneration key_generation{};
    Digest attestation_root{};

    /// The finalized state reference the epoch chains from: the certificate
    /// signing digest at Epoch 1, the committing certificate digest after.
    Digest checkpoint{};

    /// The predecessor's anchor digest; zero only at Epoch 1.
    Digest previous_anchor{};
    /// This epoch's anchor: the Genesis certificate's signing digest for
    /// Epoch 1, the finalized handoff's digest for every later epoch.
    Digest anchor_digest{};
};

/// Every field, in declaration order. Binds the persisted anchor record.
[[nodiscard]] Digest verified_epoch_authority_digest(const VerifiedEpochAuthority& authority);

/// Epoch 1 from the pinned trust anchor: the Genesis-signed certificate plus
/// the founder listing it committed to by digest. The listing is candidate
/// data; the pinned signature decides.
[[nodiscard]] std::optional<VerifiedEpochAuthority> verify_epoch_one_authority(
    const BootstrapCertificate& certificate, const crypto::Ed25519PublicKey& genesis_public_key,
    const std::vector<std::pair<NodeId, crypto::Ed25519PublicKey>>& founder_vote_keys);

enum class HandoffChainFailure : uint16_t {
    None,
    /// Another mesh, or another ruleset than this binary verifies.
    WrongNetwork,
    RulesetMismatch,
    /// Not the exact next transition: a gap, a reorder, or a replay.
    WrongEpochs,
    /// previous_anchor does not name the predecessor's anchor digest.
    LinkageBroken,
    /// The introduced membership is not a valid Tier 1 set, or its
    /// incarnation and key listings do not cover it exactly.
    MembershipInvalid,
    /// An introduced vote key or the group key repeats the previous epoch's.
    /// Keys live one epoch; a handoff certified under the keys it introduces
    /// is refused by construction.
    KeyReuse,
    KeyGenerationInvalid,
    /// The commit proof does not verify under the PREVIOUS epoch's frozen
    /// vote keys and quorum.
    ProofInvalid,
};

/// One link: advances a verified authority across one finalized handoff.
/// Returns the next epoch's authority, or the exact refusal.
[[nodiscard]] std::variant<VerifiedEpochAuthority, HandoffChainFailure> advance_epoch_authority(
    const VerifiedEpochAuthority& previous, const EpochHandoff& handoff,
    const CommitProof& proof);

}  // namespace nexus::security
