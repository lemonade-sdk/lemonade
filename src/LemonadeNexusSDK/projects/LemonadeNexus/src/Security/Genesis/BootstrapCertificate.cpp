#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>

#include <sodium.h>

namespace nexus::security {

NetworkId derive_network_id(const crypto::Ed25519PublicKey& genesis_public_key,
                            SecurityRulesetVersion security_ruleset,
                            ConsensusRulesetVersion consensus_ruleset) {
    CanonicalEncoder encoder("lemonade-nexus/network-id:v1");
    encoder.add_bytes(genesis_public_key);
    encoder.add_u16(security_ruleset);
    encoder.add_u16(consensus_ruleset);
    return encoder.digest();
}

Digest bootstrap_certificate_signing_digest(const BootstrapCertificate& certificate) {
    // v2: release_approval_digest joined the signed facts.
    CanonicalEncoder encoder("lemonade-nexus/bootstrap-certificate:v2");
    encoder.add_bytes(certificate.network_id);
    encoder.add_u64(certificate.epoch.underlying());
    encoder.add_bytes(certificate.tier1_set_digest);
    encoder.add_u64(certificate.authority_threshold);
    encoder.add_bytes(certificate.authority_public_key);
    encoder.add_bytes(certificate.dkg_transcript_digest);
    encoder.add_bytes(certificate.attestation_root);
    encoder.add_bytes(certificate.founding_eligibility_digest);
    encoder.add_bytes(certificate.vote_key_set_digest);
    encoder.add_bytes(certificate.release_approval_digest);
    encoder.add_u16(certificate.security_ruleset);
    encoder.add_u16(certificate.consensus_ruleset);
    return encoder.digest();
}

bool verify_bootstrap_certificate(const BootstrapCertificate& certificate,
                                  const crypto::Ed25519PublicKey& genesis_public_key) {
    const Digest digest = bootstrap_certificate_signing_digest(certificate);
    return crypto_sign_verify_detached(certificate.genesis_signature.data(), digest.data(),
                                       digest.size(), genesis_public_key.data()) == 0;
}

}  // namespace nexus::security
