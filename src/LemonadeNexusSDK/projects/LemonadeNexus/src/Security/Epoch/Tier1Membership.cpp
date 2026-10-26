#include <LemonadeNexus/Security/Epoch/Tier1Membership.hpp>

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>
#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <variant>

namespace nexus::security {

namespace {

[[nodiscard]] bool same_host(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

Digest tier1_endpoint_proof_digest(const NetworkId& network_id, EpochId epoch,
                                   const NodeId& node, std::string_view host, uint16_t port,
                                   std::span<const uint8_t> nonce) {
    CanonicalEncoder encoder(kTier1EndpointProofDomain);
    encoder.add_bytes(network_id);
    encoder.add_u64(epoch.underlying());
    encoder.add_bytes(node.bytes);
    // Host names compare case-insensitively, so the signed form is lower case.
    std::string lowered(host);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    encoder.add_string(lowered);
    encoder.add_u16(port);
    encoder.add_bytes(nonce);
    return encoder.digest();
}

Tier1EndpointProof sign_tier1_endpoint_proof(const NetworkId& network_id, EpochId epoch,
                                             std::string host, uint16_t port,
                                             std::span<const uint8_t> nonce,
                                             const crypto::Ed25519Keypair& identity) {
    Tier1EndpointProof proof;
    proof.node.bytes = identity.public_key;
    proof.epoch = epoch;
    proof.host = std::move(host);
    proof.port = port;
    const Digest digest =
        tier1_endpoint_proof_digest(network_id, epoch, proof.node, proof.host, port, nonce);
    crypto_sign_detached(proof.signature.data(), nullptr, digest.data(), digest.size(),
                         identity.private_key.data());
    return proof;
}

std::string_view endpoint_proof_check_name(EndpointProofCheck check) {
    switch (check) {
        case EndpointProofCheck::Valid: return "valid";
        case EndpointProofCheck::BadNonce: return "bad nonce";
        case EndpointProofCheck::NotMember: return "not a member of the verified epoch";
        case EndpointProofCheck::OlderEpoch: return "server is in an earlier epoch";
        case EndpointProofCheck::NewerEpoch: return "server is in a later epoch";
        case EndpointProofCheck::WrongEndpoint: return "proof names another endpoint";
        case EndpointProofCheck::BadSignature: return "bad signature";
    }
    return "unknown";
}

EndpointProofCheck check_tier1_endpoint_proof(const VerifiedEpochAuthority& authority,
                                              const Tier1EndpointProof& proof,
                                              std::string_view host, uint16_t port,
                                              std::span<const uint8_t> nonce) {
    if (nonce.size() != kTier1EndpointNonceBytes) {
        return EndpointProofCheck::BadNonce;
    }
    if (std::find(authority.members.begin(), authority.members.end(), proof.node) ==
        authority.members.end()) {
        return EndpointProofCheck::NotMember;
    }
    // The signature first: an epoch or endpoint claim means nothing unsigned.
    const Digest digest = tier1_endpoint_proof_digest(authority.network_id, proof.epoch,
                                                      proof.node, proof.host, proof.port, nonce);
    if (crypto_sign_verify_detached(proof.signature.data(), digest.data(), digest.size(),
                                    proof.node.bytes.data()) != 0) {
        return EndpointProofCheck::BadSignature;
    }
    if (proof.epoch < authority.epoch) {
        return EndpointProofCheck::OlderEpoch;
    }
    if (proof.epoch > authority.epoch) {
        return EndpointProofCheck::NewerEpoch;
    }
    if (proof.port != port || !same_host(proof.host, host)) {
        return EndpointProofCheck::WrongEndpoint;
    }
    return EndpointProofCheck::Valid;
}

AuthorityChainWalker::AuthorityChainWalker(const crypto::Ed25519PublicKey& genesis_public_key)
    : genesis_public_key_(genesis_public_key),
      network_id_(derive_network_id(genesis_public_key, constants::kSecurityRulesetVersion,
                                    constants::kConsensusRulesetVersion)) {}

ChainPageResult AuthorityChainWalker::apply(const AuthorityChainPage& page) {
    const EpochId before = epoch();
    if (page.has_base) {
        const Digest base_anchor = bootstrap_certificate_signing_digest(page.base_certificate);
        if (authority_.has_value()) {
            // One Genesis signs one certificate. A different base is another
            // chain, never a refresh of this one.
            if (base_anchor != epoch_one_anchor_) {
                return ChainPageResult::Refused;
            }
        } else {
            if (page.base_certificate.network_id != network_id_) {
                return ChainPageResult::Refused;
            }
            auto base = verify_epoch_one_authority(page.base_certificate, genesis_public_key_,
                                                   page.base_vote_keys);
            if (!base.has_value()) {
                return ChainPageResult::Refused;
            }
            epoch_one_anchor_ = base_anchor;
            authority_ = std::move(*base);
        }
    }
    if (!authority_.has_value()) {
        // Without the base nothing can be verified.
        return ChainPageResult::Refused;
    }
    for (const auto& link : page.links) {
        if (link.handoff.from_epoch < authority_->epoch) {
            continue;  // already verified
        }
        if (link.handoff.from_epoch != authority_->epoch) {
            return ChainPageResult::Refused;  // a gap
        }
        auto advanced = advance_epoch_authority(*authority_, link.handoff, link.proof);
        auto* next = std::get_if<VerifiedEpochAuthority>(&advanced);
        if (next == nullptr) {
            return ChainPageResult::Refused;
        }
        authority_ = std::move(*next);
    }
    return epoch() > before ? ChainPageResult::Advanced : ChainPageResult::NoProgress;
}

}  // namespace nexus::security
