#pragma once

// A portable three-chain commit proof.
//
// Chained HotStuff commits block B when B <- B1 <- B2 stand with direct parent
// links and a quorum certificate over B2. That whole structure is
// self-certifying: anyone holding the frozen membership and vote keys can
// verify it, without having been a replica. This is how a node outside the
// epoch learns that the current epoch finalized something — it verifies the
// commit itself, instead of trusting whoever delivered the bytes.
//
// The proof proves finality of one block and, through it, the transitions
// digest that block carried. It grants nothing: what the finalized record
// authorizes is the record's business.

#include <LemonadeNexus/Security/Consensus/ConsensusTypes.hpp>
#include <LemonadeNexus/Security/Consensus/QuorumValidation.hpp>

#include <map>
#include <optional>
#include <vector>

namespace nexus::security {

struct CommittedBlock {
    Proposal proposal;
    /// The certificate the proposal shipped with; it certifies the parent.
    QuorumCertificate justify;
};

struct CommitProof {
    /// Exactly three blocks: the committed block, its child, its grandchild.
    /// Under the strict parent == justify rule each child's justify certifies
    /// its parent directly.
    std::vector<CommittedBlock> chain;
    /// The certificate over the grandchild, which completes the three-chain.
    QuorumCertificate certifying;
};

enum class CommitProofFailure : uint16_t {
    None,
    WrongShape,
    WrongTransitions,
    ContextMismatch,
    BrokenChain,
    CertificateInvalid,
};

[[nodiscard]] std::string_view commit_proof_failure_name(CommitProofFailure failure);

/// Verifies that the proof commits a block carrying `transitions_digest`, under
/// the frozen membership the context and vote keys describe. Every certificate
/// is validated in full; the chain links are checked block by block.
[[nodiscard]] CommitProofFailure verify_commit_proof(
    const Digest& transitions_digest,
    const CommitProof& proof,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys);

}  // namespace nexus::security
