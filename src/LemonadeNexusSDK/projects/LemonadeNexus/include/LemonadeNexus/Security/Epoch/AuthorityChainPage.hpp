#pragma once

// The authority chain as it travels between nodes, and to clients: the pinned
// Genesis certificate with its founder listing, then finalized handoffs with
// their commit proofs. Everything here is candidate data. A receiver verifies
// it with AuthorityChain.hpp against its own pinned Genesis key.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Consensus/CommitProof.hpp>
#include <LemonadeNexus/Security/Epoch/NextEpochPlan.hpp>
#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <utility>
#include <vector>

namespace nexus::security {

/// The finalized handoff with its commit proof. Verifying it is the one and
/// only activation path for a node that was not in the old epoch.
struct EpochHandoffProofMsg {
    EpochHandoff handoff;
    CommitProof proof;
};

/// Asks any peer for the handoff chain beyond the requester's verified
/// authority. Idempotent, nonce-free, answered from local records.
struct AuthorityChainRequest {
    /// The epoch the requester's verified authority already covers; zero for
    /// a fresh node holding only the pinned Genesis anchor.
    EpochId have_epoch = 0;
};

/// One bounded page of the handoff chain, oldest link first. Candidate data:
/// the receiver verifies the base against its pinned Genesis key and every
/// link against its own advancing authority. A peer supplies bytes, never
/// trust.
struct AuthorityChainPage {
    /// Present when the page starts at Genesis: the certificate plus the
    /// epoch-1 member and vote-key listing it commits to by digest.
    bool has_base = false;
    BootstrapCertificate base_certificate;
    std::vector<std::pair<NodeId, crypto::Ed25519PublicKey>> base_vote_keys;
    /// Consecutive links, at most kMaxHandoffChainLinks per page.
    std::vector<EpochHandoffProofMsg> links;
};

}  // namespace nexus::security
