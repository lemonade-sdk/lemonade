#pragma once

// Per-epoch BFT vote key.
//
// Each Tier 1 node creates one fresh Ed25519 vote key per epoch. The key is
// separate from the node identity key and the epoch FROST share, and it dies
// with the epoch — a restored snapshot cannot vote into the next one. The
// private part lives in guarded memory and is wiped when the object goes away.
//
// Architecture reference: Security Architecture Final Draft 1.1, section 18.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Crypto/SecureBuffer.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

namespace nexus::security {

struct EpochVoteKey {
    EpochId epoch;
    NodeId node_id;

    crypto::Ed25519PublicKey public_key;
    crypto::SecureBuffer private_key;
};

[[nodiscard]] EpochVoteKey make_epoch_vote_key(EpochId epoch, const NodeId& node_id);

[[nodiscard]] crypto::Ed25519Signature sign_digest(const EpochVoteKey& key,
                                                   const Digest& digest);

[[nodiscard]] bool verify_digest(const crypto::Ed25519PublicKey& public_key,
                                 const Digest& digest,
                                 const crypto::Ed25519Signature& signature);

}  // namespace nexus::security
