#include <LemonadeNexus/Security/Consensus/VoteKey.hpp>

#include <sodium.h>

#include <stdexcept>

namespace nexus::security {

namespace {

void ensure_sodium() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

}  // namespace

EpochVoteKey make_epoch_vote_key(EpochId epoch, const NodeId& node_id) {
    ensure_sodium();
    static_assert(crypto::kEd25519PrivateKeySize == crypto_sign_SECRETKEYBYTES);
    static_assert(crypto::kEd25519PublicKeySize == crypto_sign_PUBLICKEYBYTES);

    EpochVoteKey key{};
    key.epoch = epoch;
    key.node_id = node_id;
    // The secret key is written straight into guarded memory. It never
    // touches an unguarded buffer.
    key.private_key = crypto::SecureBuffer(crypto_sign_SECRETKEYBYTES);
    if (crypto_sign_keypair(key.public_key.data(), key.private_key.data()) != 0) {
        throw std::runtime_error("Ed25519 vote key generation failed");
    }
    return key;
}

crypto::Ed25519Signature sign_digest(const EpochVoteKey& key, const Digest& digest) {
    ensure_sodium();
    if (key.private_key.size() != crypto_sign_SECRETKEYBYTES) {
        throw std::logic_error("epoch vote key has no private key material");
    }
    static_assert(crypto::kEd25519SignatureSize == crypto_sign_BYTES);

    crypto::Ed25519Signature signature{};
    if (crypto_sign_detached(signature.data(), nullptr, digest.data(), digest.size(),
                             key.private_key.data()) != 0) {
        throw std::runtime_error("Ed25519 signing failed");
    }
    return signature;
}

bool verify_digest(const crypto::Ed25519PublicKey& public_key,
                   const Digest& digest,
                   const crypto::Ed25519Signature& signature) {
    ensure_sodium();
    return crypto_sign_verify_detached(signature.data(), digest.data(), digest.size(),
                                       public_key.data()) == 0;
}

}  // namespace nexus::security
