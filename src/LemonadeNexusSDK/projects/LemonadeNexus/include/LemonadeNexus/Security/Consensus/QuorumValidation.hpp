#pragma once

// Certificate validation against frozen epoch membership.
//
// A certificate is evidence, not a best-effort tally: one bad signature
// rejects the whole certificate. The quorum in the context comes from the
// FROZEN epoch population N(E) — validation never counts reachable nodes.
//
// Architecture reference: Security Architecture Final Draft 1.1, sections 17
// and 18.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Consensus/ConsensusTypes.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstddef>
#include <map>
#include <optional>

namespace nexus::security {

struct QcValidationContext {
    ConsensusRulesetVersion consensus_ruleset;
    NetworkId network_id;
    EpochId epoch;

    // ConsensusQuorum(N(E)) of the frozen epoch population. The caller
    // computes it; validation never derives it from reachable nodes.
    std::size_t quorum;
};

// nullopt means valid. Checks run cheap before expensive: header fields and
// signer bounds reject before any signature verification.
[[nodiscard]] std::optional<ConsensusFailure> validate_quorum_certificate(
    const QuorumCertificate& certificate,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys);

// Each signer signs its own timeout digest with its own high_qc_digest.
[[nodiscard]] std::optional<ConsensusFailure> validate_timeout_certificate(
    const TimeoutCertificate& certificate,
    const QcValidationContext& context,
    const std::map<NodeId, crypto::Ed25519PublicKey>& epoch_vote_keys);

}  // namespace nexus::security
