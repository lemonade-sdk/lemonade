#pragma once

// The fresh proof an account deletion request carries (POST
// /api/account/delete). The server and the client SDK build the signed bytes
// with this one function, so both sign and verify the same encoding.

#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nexus::auth {

/// Domain of the account deletion proof. It separates the proof from a login
/// signature: a signature over a login challenge never deletes an account,
/// and a deletion proof never opens a session.
inline constexpr std::string_view kAccountDeleteProofDomain = "lemonade-nexus/account-delete:v1";

/// The bytes the account key signs to delete its account: the canonical
/// encoding (domain kAccountDeleteProofDomain) of the raw challenge bytes the
/// server issued and the raw 32-byte Ed25519 public key of the account.
/// The challenge is single use and expires (Ed25519AuthProvider), so a proof
/// cannot be replayed.
[[nodiscard]] inline std::vector<uint8_t> account_delete_proof_message(
        std::span<const uint8_t> challenge, std::span<const uint8_t> account_pubkey) {
    security::CanonicalEncoder enc(kAccountDeleteProofDomain);
    enc.add_bytes(challenge);
    enc.add_bytes(account_pubkey);
    return enc.bytes();
}

}  // namespace nexus::auth
