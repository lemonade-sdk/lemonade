#pragma once

// Chained HotStuff wire types and their canonical digests.
//
// These types are plain data. They carry no transport concerns and grant no
// authority — a certificate is evidence that validation must examine.
//
// Every digest goes through CanonicalEncoder under kBftProtocolDomain with a
// kind string as the second field, so no two message kinds can ever collide.
//
// Architecture reference: Security Architecture Final Draft 1.1, section 17.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstdint>
#include <vector>

namespace nexus::security {

struct Proposal {
    SecurityRulesetVersion security_ruleset;
    ConsensusRulesetVersion consensus_ruleset;

    NetworkId network_id;
    EpochId epoch;

    Height height;
    View view;

    NodeId leader;

    Digest parent_digest;
    Digest justify_qc_digest;

    Digest previous_state_root;
    Digest proposed_state_root;
    Digest transitions_digest;

    // Informational only. It is inside the digest, but it MUST never drive a
    // safety decision.
    uint64_t timestamp_hint;
};

struct Vote {
    ConsensusRulesetVersion consensus_ruleset;
    NetworkId network_id;
    EpochId epoch;
    Height height;
    View view;
    Digest proposal_digest;
    NodeId voter;
    crypto::Ed25519Signature signature;
};

struct TimeoutVote {
    ConsensusRulesetVersion consensus_ruleset;
    NetworkId network_id;
    EpochId epoch;
    View view;
    Digest high_qc_digest;
    NodeId voter;
    crypto::Ed25519Signature signature;
};

struct QcSigner {
    NodeId node_id;
    crypto::Ed25519Signature signature;
};

struct QuorumCertificate {
    uint16_t qc_format_version;
    ConsensusRulesetVersion consensus_ruleset;
    NetworkId network_id;
    EpochId epoch;
    Height height;
    View view;
    Digest proposal_digest;
    std::vector<QcSigner> signers;
};

// Each timeout signer attests to its own highest known QC.
struct TimeoutSigner {
    NodeId node_id;
    Digest high_qc_digest;
    crypto::Ed25519Signature signature;
};

struct TimeoutCertificate {
    ConsensusRulesetVersion consensus_ruleset;
    NetworkId network_id;
    EpochId epoch;
    View view;
    std::vector<TimeoutSigner> signers;
};

struct ConsensusCommit {
    EpochId epoch;
    Height height;
    View view;
    Digest proposal_digest;
    Digest proposed_state_root;
    Digest transitions_digest{};
    Digest qc_digest;
};

// Digests of the durable store records. They are what a restarted node
// re-checks against its own file before acting on it: the epoch store binds
// its anchor the same way (record_digest), so a consensus safety or commit
// file flipped at rest loads as Corrupt, never as different state.
//
// The encoder is the same one every other consensus digest uses
// (CanonicalEncoder under kBftProtocolDomain, kind string second); the
// records bind the exact field set of their on-disk form, including the
// format version.
[[nodiscard]] Digest consensus_state_record_digest(const QuorumCertificate& high_qc,
                                                  const QuorumCertificate& locked_qc,
                                                  ConsensusRulesetVersion consensus_ruleset,
                                                  EpochId epoch,
                                                  View last_voted_view);
[[nodiscard]] Digest consensus_commit_record_digest(const ConsensusCommit& commit);

enum class ConsensusFailure : uint16_t {
    FormatVersion,
    RulesetMismatch,
    NetworkMismatch,
    EpochMismatch,
    ViewMismatch,
    TooManySignatures,
    DuplicateSigner,
    UnknownSigner,
    InvalidSignature,
    InsufficientQuorum,
    ProposalDigestMismatch,
    // Service-level failures. Values append only; earlier values are stable.
    NotSynced,
    StaleView,
    ViewTooFar,
    WrongLeader,
    JustifyInvalid,
    ParentQcMismatch,
    MissingParent,
    Equivocation,
    PendingLimit,
    NotLeader,
    StorageRejected,
    /// Retired: an unknown transition now withholds the vote instead of
    /// rejecting the block. The value stays; earlier values are stable.
    TransitionUnknown,
};

[[nodiscard]] Digest proposal_digest(const Proposal& proposal);

[[nodiscard]] Digest vote_signing_digest(ConsensusRulesetVersion consensus_ruleset,
                                         const NetworkId& network_id,
                                         EpochId epoch,
                                         Height height,
                                         View view,
                                         const Digest& proposal_digest,
                                         const NodeId& voter);
[[nodiscard]] Digest vote_signing_digest(const Vote& vote);

[[nodiscard]] Digest timeout_vote_signing_digest(ConsensusRulesetVersion consensus_ruleset,
                                                 const NetworkId& network_id,
                                                 EpochId epoch,
                                                 View view,
                                                 const Digest& high_qc_digest,
                                                 const NodeId& voter);
[[nodiscard]] Digest timeout_vote_signing_digest(const TimeoutVote& vote);

// Certificate digests sort signers ascending by node identity, so the digest
// depends on the signer set and never on arrival order.
[[nodiscard]] Digest qc_digest(const QuorumCertificate& certificate);

[[nodiscard]] Digest timeout_certificate_digest(const TimeoutCertificate& certificate);

}  // namespace nexus::security
