#include <LemonadeNexus/Security/Consensus/ConsensusTypes.hpp>

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include <algorithm>

namespace nexus::security {

namespace {

[[nodiscard]] CanonicalEncoder make_encoder(std::string_view kind) {
    CanonicalEncoder encoder(constants::kBftProtocolDomain);
    encoder.add_string(kind);
    return encoder;
}

}  // namespace

Digest proposal_digest(const Proposal& proposal) {
    auto encoder = make_encoder("proposal");
    encoder.add_u16(proposal.security_ruleset);
    encoder.add_u16(proposal.consensus_ruleset);
    encoder.add_bytes(proposal.network_id);
    encoder.add_u64(proposal.epoch.underlying());
    encoder.add_u64(proposal.height.underlying());
    encoder.add_u64(proposal.view.underlying());
    encoder.add_bytes(proposal.leader.span());
    encoder.add_bytes(proposal.parent_digest);
    encoder.add_bytes(proposal.justify_qc_digest);
    encoder.add_bytes(proposal.previous_state_root);
    encoder.add_bytes(proposal.proposed_state_root);
    encoder.add_bytes(proposal.transitions_digest);
    encoder.add_u64(proposal.timestamp_hint);
    return encoder.digest();
}

Digest vote_signing_digest(ConsensusRulesetVersion consensus_ruleset,
                           const NetworkId& network_id,
                           EpochId epoch,
                           Height height,
                           View view,
                           const Digest& proposal_digest,
                           const NodeId& voter) {
    auto encoder = make_encoder("vote");
    encoder.add_u16(consensus_ruleset);
    encoder.add_bytes(network_id);
    encoder.add_u64(epoch.underlying());
    encoder.add_u64(height.underlying());
    encoder.add_u64(view.underlying());
    encoder.add_bytes(proposal_digest);
    encoder.add_bytes(voter.span());
    return encoder.digest();
}

Digest vote_signing_digest(const Vote& vote) {
    return vote_signing_digest(vote.consensus_ruleset, vote.network_id, vote.epoch,
                               vote.height, vote.view, vote.proposal_digest, vote.voter);
}

Digest timeout_vote_signing_digest(ConsensusRulesetVersion consensus_ruleset,
                                   const NetworkId& network_id,
                                   EpochId epoch,
                                   View view,
                                   const Digest& high_qc_digest,
                                   const NodeId& voter) {
    auto encoder = make_encoder("timeout-vote");
    encoder.add_u16(consensus_ruleset);
    encoder.add_bytes(network_id);
    encoder.add_u64(epoch.underlying());
    encoder.add_u64(view.underlying());
    encoder.add_bytes(high_qc_digest);
    encoder.add_bytes(voter.span());
    return encoder.digest();
}

Digest timeout_vote_signing_digest(const TimeoutVote& vote) {
    return timeout_vote_signing_digest(vote.consensus_ruleset, vote.network_id,
                                       vote.epoch, vote.view, vote.high_qc_digest,
                                       vote.voter);
}

Digest qc_digest(const QuorumCertificate& certificate) {
    auto encoder = make_encoder("qc");
    encoder.add_u16(certificate.qc_format_version);
    encoder.add_u16(certificate.consensus_ruleset);
    encoder.add_bytes(certificate.network_id);
    encoder.add_u64(certificate.epoch.underlying());
    encoder.add_u64(certificate.height.underlying());
    encoder.add_u64(certificate.view.underlying());
    encoder.add_bytes(certificate.proposal_digest);

    auto signers = certificate.signers;
    std::sort(signers.begin(), signers.end(), [](const QcSigner& a, const QcSigner& b) {
        if (a.node_id != b.node_id) return a.node_id < b.node_id;
        return a.signature < b.signature;
    });

    encoder.add_u64(static_cast<uint64_t>(signers.size()));
    for (const auto& signer : signers) {
        encoder.add_bytes(signer.node_id.span());
        encoder.add_bytes(signer.signature);
    }
    return encoder.digest();
}

Digest timeout_certificate_digest(const TimeoutCertificate& certificate) {
    auto encoder = make_encoder("tc");
    encoder.add_u16(certificate.consensus_ruleset);
    encoder.add_bytes(certificate.network_id);
    encoder.add_u64(certificate.epoch.underlying());
    encoder.add_u64(certificate.view.underlying());

    auto signers = certificate.signers;
    std::sort(signers.begin(), signers.end(),
              [](const TimeoutSigner& a, const TimeoutSigner& b) {
                  if (a.node_id != b.node_id) return a.node_id < b.node_id;
                  if (a.high_qc_digest != b.high_qc_digest) {
                      return a.high_qc_digest < b.high_qc_digest;
                  }
                  return a.signature < b.signature;
              });

    encoder.add_u64(static_cast<uint64_t>(signers.size()));
    for (const auto& signer : signers) {
        encoder.add_bytes(signer.node_id.span());
        encoder.add_bytes(signer.high_qc_digest);
        encoder.add_bytes(signer.signature);
    }
    return encoder.digest();
}

Digest consensus_state_record_digest(const QuorumCertificate& high_qc,
                                     const QuorumCertificate& locked_qc,
                                     ConsensusRulesetVersion consensus_ruleset, EpochId epoch,
                                     View last_voted_view) {
    auto encoder = make_encoder("safety-record");
    encoder.add_u32(constants::kConsensusStoreFormatVersion);
    encoder.add_u64(epoch.underlying());
    encoder.add_u16(consensus_ruleset);
    encoder.add_u64(last_voted_view.underlying());
    encoder.add_bytes(qc_digest(high_qc));
    encoder.add_bytes(qc_digest(locked_qc));
    return encoder.digest();
}

Digest consensus_commit_record_digest(const ConsensusCommit& commit) {
    auto encoder = make_encoder("commit-record");
    encoder.add_u32(constants::kConsensusStoreFormatVersion);
    encoder.add_u64(commit.epoch.underlying());
    encoder.add_u64(commit.height.underlying());
    encoder.add_u64(commit.view.underlying());
    encoder.add_bytes(commit.proposal_digest);
    encoder.add_bytes(commit.proposed_state_root);
    encoder.add_bytes(commit.transitions_digest);
    encoder.add_bytes(commit.qc_digest);
    return encoder.digest();
}

}  // namespace nexus::security
