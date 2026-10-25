#pragma once

// Tier 1 membership for parties outside the epoch: clients and the SDK.
//
// A client trusts only its pinned Genesis key. It learns the current Tier 1
// set by walking the authority chain (AuthorityChain.hpp) from that key, page
// by page, exactly as a Tier 2 server does. It then learns where a member
// answers from the member itself: a signed endpoint proof over a client nonce,
// bound to the network, the epoch, and the host and port the client reached.
// A server that is not in the verified set, or that proves a different epoch
// or endpoint, is not a Tier 1 endpoint for that client.
//
// Everything here is pure verification over supplied bytes, with no I/O and
// no clock. The server and the SDK both compile this file.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Epoch/AuthorityChain.hpp>
#include <LemonadeNexus/Security/Epoch/AuthorityChainPage.hpp>
#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nexus::security {

/// Domain of the endpoint proof digest. The proof is signed with the member's
/// node identity key (its NodeId), under this domain only.
inline constexpr std::string_view kTier1EndpointProofDomain =
    "lemonade-nexus/tier1-endpoint-proof:v1";

/// Exact size of the client nonce an endpoint proof answers.
inline constexpr std::size_t kTier1EndpointNonceBytes = 32;

/// Longest host name an endpoint proof can carry (a DNS name).
inline constexpr std::size_t kTier1EndpointMaxHostBytes = 253;

/// A Tier 1 member's statement that it answers at `host`:`port` in `epoch`,
/// signed over tier1_endpoint_proof_digest().
struct Tier1EndpointProof {
    NodeId node{};
    EpochId epoch{};
    std::string host;
    uint16_t port = 0;
    crypto::Ed25519Signature signature{};
};

/// The signed digest: network, epoch, node, host, port, and the client nonce.
[[nodiscard]] Digest tier1_endpoint_proof_digest(const NetworkId& network_id, EpochId epoch,
                                                 const NodeId& node, std::string_view host,
                                                 uint16_t port,
                                                 std::span<const uint8_t> nonce);

/// Signs an endpoint proof with `identity`, the member's node identity key.
/// The node in the proof is `identity.public_key`.
[[nodiscard]] Tier1EndpointProof sign_tier1_endpoint_proof(
    const NetworkId& network_id, EpochId epoch, std::string host, uint16_t port,
    std::span<const uint8_t> nonce, const crypto::Ed25519Keypair& identity);

enum class EndpointProofCheck : uint8_t {
    Valid,
    /// The nonce is not kTier1EndpointNonceBytes long.
    BadNonce,
    /// The signer is not in the verified membership.
    NotMember,
    /// The server is in an earlier epoch than the verified one.
    OlderEpoch,
    /// The server proves a later epoch: the verified membership is stale and
    /// the chain must be walked further before this answer is used.
    NewerEpoch,
    /// The proof names another host or port than the one the client reached.
    WrongEndpoint,
    BadSignature,
};

[[nodiscard]] std::string_view endpoint_proof_check_name(EndpointProofCheck check);

/// Checks `proof` against the verified authority, the endpoint the client
/// reached (`host` compares case-insensitively), and the nonce it sent.
[[nodiscard]] EndpointProofCheck check_tier1_endpoint_proof(
    const VerifiedEpochAuthority& authority, const Tier1EndpointProof& proof,
    std::string_view host, uint16_t port, std::span<const uint8_t> nonce);

enum class ChainPageResult : uint8_t {
    /// The verified epoch moved forward.
    Advanced,
    /// Nothing new: an empty page, or links the walker already holds.
    NoProgress,
    /// The page does not verify: a forged or foreign base, a gap, or a link
    /// whose proof fails. Links verified before the failure stay applied.
    Refused,
};

/// Walks the authority chain from a pinned Genesis key. Pages are candidate
/// data; only verify_epoch_one_authority and advance_epoch_authority move the
/// walker. The verified epoch never decreases. Not thread-safe.
class AuthorityChainWalker {
public:
    explicit AuthorityChainWalker(const crypto::Ed25519PublicKey& genesis_public_key);

    /// Applies one page. The first page must carry the Genesis base, and the
    /// base certificate must name the network this Genesis key derives.
    ChainPageResult apply(const AuthorityChainPage& page);

    /// The verified authority, or nullopt before a base verified.
    [[nodiscard]] const std::optional<VerifiedEpochAuthority>& authority() const {
        return authority_;
    }

    /// The verified epoch, or 0 before a base verified.
    [[nodiscard]] EpochId epoch() const { return authority_ ? authority_->epoch : EpochId{0}; }

    /// The network the pinned Genesis key derives.
    [[nodiscard]] const NetworkId& network_id() const { return network_id_; }

private:
    crypto::Ed25519PublicKey genesis_public_key_{};
    NetworkId network_id_{};
    Digest epoch_one_anchor_{};
    std::optional<VerifiedEpochAuthority> authority_;
};

}  // namespace nexus::security
