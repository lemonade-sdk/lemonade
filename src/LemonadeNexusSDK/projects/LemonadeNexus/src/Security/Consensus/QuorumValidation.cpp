#include <LemonadeNexus/Security/Consensus/QuorumValidation.hpp>

#include <LemonadeNexus/Security/Consensus/VoteKey.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include <set>
#include <vector>

namespace nexus::security {

namespace {

// Duplicates from one node_id count once — a cloned identity gains no voting
// weight, and it does not invalidate the certificate either. Keep the first
// entry per node_id.
template <typename Signer>
[[nodiscard]] std::vector<const Signer*> distinct_signers(const std::vector<Signer>& signers) {
    std::set<NodeId> seen;
    std::vector<const Signer*> distinct;
    distinct.reserve(signers.size());
    for (const auto& signer : signers) {
        if (seen.insert(signer.node_id).second) {
            distinct.push_back(&signer);
        }
    }
    return distinct;
}

}  // namespace

std::optional<ConsensusFailure> validate_quorum_certificate(
    const QuorumCertificate& certificate,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys) {
    if (certificate.qc_format_version != constants::kQcFormatVersion) {
        return ConsensusFailure::FormatVersion;
    }
    if (certificate.consensus_ruleset != context.consensus_ruleset) {
        return ConsensusFailure::RulesetMismatch;
    }
    if (certificate.network_id != context.network_id) {
        return ConsensusFailure::NetworkMismatch;
    }
    if (certificate.epoch != context.epoch) {
        return ConsensusFailure::EpochMismatch;
    }
    if (certificate.signers.size() > constants::kMaxQcSignatures) {
        return ConsensusFailure::TooManySignatures;
    }

    // Frozen membership: a signer outside the epoch vote-key set rejects the
    // certificate. Membership never grows to fit evidence.
    for (const auto& signer : certificate.signers) {
        if (!epoch_vote_keys.contains(signer.node_id)) {
            return ConsensusFailure::UnknownSigner;
        }
    }

    const auto distinct = distinct_signers(certificate.signers);
    if (distinct.size() < context.quorum) {
        return ConsensusFailure::InsufficientQuorum;
    }

    for (const auto* signer : distinct) {
        const auto digest = vote_signing_digest(
            certificate.consensus_ruleset, certificate.network_id, certificate.epoch,
            certificate.height, certificate.view, certificate.proposal_digest,
            signer->node_id);
        if (!verify_digest(epoch_vote_keys.at(signer->node_id), digest,
                           signer->signature)) {
            return ConsensusFailure::InvalidSignature;
        }
    }
    return std::nullopt;
}

std::optional<ConsensusFailure> validate_timeout_certificate(
    const TimeoutCertificate& certificate,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys) {
    if (certificate.consensus_ruleset != context.consensus_ruleset) {
        return ConsensusFailure::RulesetMismatch;
    }
    if (certificate.network_id != context.network_id) {
        return ConsensusFailure::NetworkMismatch;
    }
    if (certificate.epoch != context.epoch) {
        return ConsensusFailure::EpochMismatch;
    }
    if (certificate.signers.size() > constants::kMaxQcSignatures) {
        return ConsensusFailure::TooManySignatures;
    }

    for (const auto& signer : certificate.signers) {
        if (!epoch_vote_keys.contains(signer.node_id)) {
            return ConsensusFailure::UnknownSigner;
        }
    }

    const auto distinct = distinct_signers(certificate.signers);
    if (distinct.size() < context.quorum) {
        return ConsensusFailure::InsufficientQuorum;
    }

    for (const auto* signer : distinct) {
        const auto digest = timeout_vote_signing_digest(
            certificate.consensus_ruleset, certificate.network_id, certificate.epoch,
            certificate.view, signer->high_qc_digest, signer->node_id);
        if (!verify_digest(epoch_vote_keys.at(signer->node_id), digest,
                           signer->signature)) {
            return ConsensusFailure::InvalidSignature;
        }
    }
    return std::nullopt;
}

}  // namespace nexus::security
