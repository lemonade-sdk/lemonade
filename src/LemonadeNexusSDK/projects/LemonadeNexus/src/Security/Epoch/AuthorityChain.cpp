#include <LemonadeNexus/Security/Epoch/AuthorityChain.hpp>

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>
#include <LemonadeNexus/Security/Consensus/QuorumValidation.hpp>
#include <LemonadeNexus/Security/Epoch/Tier1Set.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include <algorithm>

namespace nexus::security {

namespace {

inline constexpr std::string_view kVerifiedAuthorityDomain =
    "lemonade-nexus/verified-epoch-authority:v1";

}  // namespace

Digest verified_epoch_authority_digest(const VerifiedEpochAuthority& authority) {
    CanonicalEncoder encoder(kVerifiedAuthorityDomain);
    encoder.add_bytes(authority.network_id);
    encoder.add_u64(authority.epoch.underlying());
    encoder.add_u64(authority.members.size());
    for (const auto& node : authority.members) {
        encoder.add_bytes(node.bytes);
        const auto incarnation = authority.incarnations.find(node);
        encoder.add_u64(incarnation != authority.incarnations.end() ? incarnation->second.underlying() : 0u);
    }
    encoder.add_bytes(vote_key_set_digest(authority.vote_keys));
    encoder.add_u64(static_cast<uint64_t>(authority.consensus_quorum));
    encoder.add_u64(static_cast<uint64_t>(authority.authority_threshold));
    encoder.add_u16(authority.security_ruleset);
    encoder.add_u16(authority.consensus_ruleset);
    encoder.add_bytes(authority.group_public_key);
    encoder.add_u64(authority.key_generation.underlying());
    encoder.add_bytes(authority.attestation_root);
    encoder.add_bytes(authority.checkpoint);
    encoder.add_bytes(authority.previous_anchor);
    encoder.add_bytes(authority.anchor_digest);
    return encoder.digest();
}

std::optional<VerifiedEpochAuthority> verify_epoch_one_authority(
    const BootstrapCertificate& certificate, const crypto::Ed25519PublicKey& genesis_public_key,
    const std::vector<std::pair<NodeId, crypto::Ed25519PublicKey>>& founder_vote_keys) {
    // The pinned signature first: nothing below it means anything without it.
    if (!verify_bootstrap_certificate(certificate, genesis_public_key)) {
        return std::nullopt;
    }
    if (certificate.epoch != 1 ||
        certificate.security_ruleset != constants::kSecurityRulesetVersion ||
        certificate.consensus_ruleset != constants::kConsensusRulesetVersion) {
        return std::nullopt;
    }

    // The supplied listing must hash to exactly what Genesis signed — the
    // membership by set digest, the keys by key-set digest.
    std::vector<NodeId> members;
    std::map<NodeId, crypto::Ed25519PublicKey> keys;
    for (const auto& [node, key] : founder_vote_keys) {
        members.push_back(node);
        keys[node] = key;
    }
    const auto founders = Tier1Set::from_nodes(members);
    if (!founders.has_value() || keys.size() != founders->size() ||
        founders->digest() != certificate.tier1_set_digest ||
        vote_key_set_digest(keys) != certificate.vote_key_set_digest) {
        return std::nullopt;
    }
    if (certificate.authority_threshold != constants::authority_threshold(founders->size())) {
        return std::nullopt;
    }

    VerifiedEpochAuthority authority;
    authority.network_id = certificate.network_id;
    authority.epoch = 1;
    authority.members = founders->members();
    for (const auto& node : authority.members) {
        authority.incarnations[node] = 1;
    }
    authority.vote_keys = std::move(keys);
    authority.consensus_quorum = constants::consensus_quorum(founders->size());
    authority.authority_threshold = certificate.authority_threshold;
    authority.security_ruleset = certificate.security_ruleset;
    authority.consensus_ruleset = certificate.consensus_ruleset;
    authority.group_public_key = certificate.authority_public_key;
    authority.key_generation = 1;
    authority.attestation_root = certificate.attestation_root;
    authority.checkpoint = bootstrap_certificate_signing_digest(certificate);
    authority.previous_anchor = Digest{};
    authority.anchor_digest = authority.checkpoint;
    return authority;
}

std::variant<VerifiedEpochAuthority, HandoffChainFailure> advance_epoch_authority(
    const VerifiedEpochAuthority& previous, const EpochHandoff& handoff,
    const CommitProof& proof) {
    if (handoff.network_id != previous.network_id) {
        return HandoffChainFailure::WrongNetwork;
    }
    // This binary verifies its own compiled rules and no others.
    if (handoff.security_ruleset != constants::kSecurityRulesetVersion ||
        handoff.consensus_ruleset != constants::kConsensusRulesetVersion) {
        return HandoffChainFailure::RulesetMismatch;
    }
    // Exactly the next transition. A gap, a reorder, and a replay of an
    // earlier epoch all die on the same arithmetic.
    if (handoff.from_epoch != previous.epoch || handoff.to_epoch != previous.epoch + 1) {
        return HandoffChainFailure::WrongEpochs;
    }
    // And chained to exactly this predecessor's anchor.
    if (handoff.previous_anchor != previous.anchor_digest) {
        return HandoffChainFailure::LinkageBroken;
    }

    const auto members = Tier1Set::from_nodes(handoff.members);
    if (!members.has_value() || members->size() < constants::kMinActiveTier1 ||
        handoff.vote_keys.size() != members->size() ||
        handoff.incarnations.size() != members->size()) {
        return HandoffChainFailure::MembershipInvalid;
    }
    for (const auto& node : members->members()) {
        if (!handoff.vote_keys.contains(node) || !handoff.incarnations.contains(node)) {
            return HandoffChainFailure::MembershipInvalid;
        }
    }

    // Keys live one epoch. An introduced vote key that repeats a previous
    // one — or the previous group key surviving — would let old material act
    // in the new epoch, and a handoff certified under the keys it introduces
    // is exactly the self-certification this walk exists to refuse.
    if (handoff.group_public_key == previous.group_public_key) {
        return HandoffChainFailure::KeyReuse;
    }
    for (const auto& [node, key] : handoff.vote_keys) {
        for (const auto& [previous_node, previous_key] : previous.vote_keys) {
            if (key == previous_key) {
                return HandoffChainFailure::KeyReuse;
            }
        }
    }
    if (handoff.key_generation != KeyGeneration(handoff.to_epoch.underlying())) {
        return HandoffChainFailure::KeyGenerationInvalid;
    }

    // The proof must commit this exact handoff under the PREVIOUS epoch's
    // frozen vote keys and quorum. New keys certify nothing here.
    const QcValidationContext context{constants::kConsensusRulesetVersion, previous.network_id,
                                      previous.epoch, previous.consensus_quorum};
    if (verify_commit_proof(epoch_handoff_digest(handoff), proof, context,
                            previous.vote_keys) != CommitProofFailure::None) {
        return HandoffChainFailure::ProofInvalid;
    }

    VerifiedEpochAuthority next;
    next.network_id = previous.network_id;
    next.epoch = handoff.to_epoch;
    next.members = members->members();
    next.incarnations = handoff.incarnations;
    next.vote_keys = handoff.vote_keys;
    next.consensus_quorum = constants::consensus_quorum(members->size());
    next.authority_threshold = constants::authority_threshold(members->size());
    next.security_ruleset = handoff.security_ruleset;
    next.consensus_ruleset = handoff.consensus_ruleset;
    next.group_public_key = handoff.group_public_key;
    next.key_generation = handoff.key_generation;
    next.attestation_root = handoff.attestation_root;
    // The same finalized reference an activating newcomer chains from: the
    // certificate over the block that carried the handoff.
    next.checkpoint = qc_digest(proof.chain[1].justify);
    next.previous_anchor = previous.anchor_digest;
    next.anchor_digest = epoch_handoff_digest(handoff);
    return next;
}

}  // namespace nexus::security
