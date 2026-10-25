#pragma once

// The one-time bridge from Genesis authority to Epoch 1.
//
// Genesis signs exactly one certificate, over the finalized Epoch 1 facts.
// Epoch 1 consumes it; later epochs chain from epoch authority records and
// never look at Genesis again. There is no post-genesis override.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/CanonicalEncoding.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstddef>

namespace nexus::security {

// Fixed before the certificate exists: every later object binds to this value,
// including the certificate itself.
[[nodiscard]] NetworkId derive_network_id(const crypto::Ed25519PublicKey& genesis_public_key,
                                          SecurityRulesetVersion security_ruleset,
                                          ConsensusRulesetVersion consensus_ruleset);

struct BootstrapCertificate {
    NetworkId network_id{};

    EpochId epoch = 1;

    Digest tier1_set_digest{};
    std::size_t authority_threshold = 0;

    crypto::Ed25519PublicKey authority_public_key{};

    Digest dkg_transcript_digest{};
    Digest attestation_root{};

    /// The founding eligibility state every founder independently computed and
    /// signed. Genesis can only copy the value the founders agreed on, so the
    /// certificate cannot admit a founder the founding set refused.
    Digest founding_eligibility_digest{};

    /// The founders' epoch-1 BFT vote keys, as one digest. This is the anchor a
    /// later candidate verifies a supplied key listing against: membership by
    /// tier1_set_digest, keys by this, both under the pinned genesis signature.
    Digest vote_key_set_digest{};

    /// The accepted release approval, as the digest of its canonical form.
    /// Committed here so the selection survives Genesis: after Epoch 1 a
    /// different Genesis-signed approval cannot replace it. All-zero means the
    /// network bootstrapped without a release binding (test meshes only).
    Digest release_approval_digest{};

    SecurityRulesetVersion security_ruleset = 0;
    ConsensusRulesetVersion consensus_ruleset = 0;

    crypto::Ed25519Signature genesis_signature{};
};

[[nodiscard]] Digest bootstrap_certificate_signing_digest(const BootstrapCertificate& certificate);

[[nodiscard]] bool verify_bootstrap_certificate(const BootstrapCertificate& certificate,
                                                const crypto::Ed25519PublicKey& genesis_public_key);

}  // namespace nexus::security
