#include <LemonadeNexus/Security/Consensus/CommitProof.hpp>

namespace nexus::security {

std::string_view commit_proof_failure_name(CommitProofFailure failure) {
    switch (failure) {
        case CommitProofFailure::None:               return "none";
        case CommitProofFailure::WrongShape:         return "wrong shape";
        case CommitProofFailure::WrongTransitions:   return "wrong transitions digest";
        case CommitProofFailure::ContextMismatch:    return "wrong network, epoch or ruleset";
        case CommitProofFailure::BrokenChain:        return "broken chain";
        case CommitProofFailure::CertificateInvalid: return "certificate invalid";
    }
    return "unknown failure";
}

CommitProofFailure verify_commit_proof(
    const Digest& transitions_digest, const CommitProof& proof,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys) {
    if (proof.chain.size() != 3) {
        return CommitProofFailure::WrongShape;
    }
    const Proposal& committed = proof.chain[0].proposal;
    if (committed.transitions_digest != transitions_digest) {
        return CommitProofFailure::WrongTransitions;
    }

    for (const auto& block : proof.chain) {
        const Proposal& proposal = block.proposal;
        if (proposal.network_id != context.network_id || proposal.epoch != context.epoch ||
            proposal.consensus_ruleset != context.consensus_ruleset) {
            return CommitProofFailure::ContextMismatch;
        }
    }

    // The strict parent == justify rule: each child's justify certifies its
    // parent, heights are consecutive, and the final certificate covers the
    // grandchild. This is the exact structure the commit rule demands.
    for (std::size_t i = 0; i + 1 < proof.chain.size(); ++i) {
        const Digest parent = proposal_digest(proof.chain[i].proposal);
        const auto& child = proof.chain[i + 1];
        if (child.justify.proposal_digest != parent ||
            child.proposal.parent_digest != parent ||
            child.proposal.height != proof.chain[i].proposal.height + 1) {
            return CommitProofFailure::BrokenChain;
        }
    }
    if (proof.certifying.proposal_digest != proposal_digest(proof.chain[2].proposal)) {
        return CommitProofFailure::BrokenChain;
    }

    // Every certificate in full: the two inner justifies and the closing one.
    // The committed block's own justify certifies state before the proof and
    // is not needed for the three-chain, so it is not judged here.
    for (const QuorumCertificate* certificate :
         {&proof.chain[1].justify, &proof.chain[2].justify, &proof.certifying}) {
        if (validate_quorum_certificate(*certificate, context, epoch_vote_keys).has_value()) {
            return CommitProofFailure::CertificateInvalid;
        }
    }
    return CommitProofFailure::None;
}

}  // namespace nexus::security
