#pragma once

#include <array>
#include <string_view>
#include <initializer_list>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nexus::crypto {

// --- Ed25519 ---
static constexpr std::size_t kEd25519PublicKeySize  = 32;
static constexpr std::size_t kEd25519PrivateKeySize = 64; // libsodium stores seed+pk
static constexpr std::size_t kEd25519SeedSize       = 32;
static constexpr std::size_t kEd25519SignatureSize   = 64;

using Ed25519PublicKey  = std::array<uint8_t, kEd25519PublicKeySize>;
using Ed25519PrivateKey = std::array<uint8_t, kEd25519PrivateKeySize>;
using Ed25519Seed       = std::array<uint8_t, kEd25519SeedSize>;
using Ed25519Signature  = std::array<uint8_t, kEd25519SignatureSize>;

struct Ed25519Keypair {
    Ed25519PublicKey  public_key{};
    Ed25519PrivateKey private_key{};
};

// --- X25519 (Diffie-Hellman) ---
static constexpr std::size_t kX25519PublicKeySize  = 32;
static constexpr std::size_t kX25519PrivateKeySize = 32;
static constexpr std::size_t kX25519SharedSize     = 32;

using X25519PublicKey  = std::array<uint8_t, kX25519PublicKeySize>;
using X25519PrivateKey = std::array<uint8_t, kX25519PrivateKeySize>;
using X25519SharedSecret = std::array<uint8_t, kX25519SharedSize>;

struct X25519Keypair {
    X25519PublicKey  public_key{};
    X25519PrivateKey private_key{};
};

// --- Application AEAD: XChaCha20-Poly1305-IETF, and nothing else -------------
//
// One algorithm on every cpu. The cipher is NOT chosen from hardware
// capability: a ciphertext written anywhere must open anywhere with the same
// key, so AES-NI, cpu model, VM host and architecture are not inputs to the
// format. libsodium ships AES-256-GCM only as aesni/armcrypto, so an
// AES-encrypted object is unreadable on a machine without those instructions —
// which is exactly the property this rules out.
static constexpr std::size_t kAeadKeySize   = 32;
static constexpr std::size_t kAeadNonceSize = 24;
static constexpr std::size_t kAeadTagSize   = 16;
using AeadKey = std::array<uint8_t, kAeadKeySize>;

/// The one encrypted-object format. `version` names an exact construction, so a
/// reader never infers the algorithm from field widths.
///
///   version 1 = XChaCha20-Poly1305-IETF, 24-byte random nonce, 16-byte tag
///
/// Unknown versions fail closed. There is deliberately no algorithm field:
/// negotiation would put the choice back on the wire.
static constexpr uint8_t kEncryptedBlobVersion = 1;

struct EncryptedBlob {
    uint8_t              version{kEncryptedBlobVersion};
    std::vector<uint8_t> nonce;       // exactly kAeadNonceSize
    std::vector<uint8_t> ciphertext;  // includes the appended tag
};

/// Canonical authenticated associated data for one encrypted object:
///
///     version || LP(purpose) || LP(context[0]) || LP(context[1]) ...
///
/// where LP(x) is a u32 little-endian length followed by the bytes. Every field
/// is length-prefixed, so no two (purpose, context) pairs can produce the same
/// AAD by moving a boundary.
///
/// The version is the COMPILED constant, not the value read off a blob. A
/// tampered version field is already refused before decryption, and binding the
/// constant is what stops a future version-2 ciphertext from being replayed as
/// version 1: its AAD was computed with a different leading byte, so the tag
/// fails. This is domain separation, not algorithm negotiation.
///
/// `purpose` is a fixed tag per construction; `context` carries the immutable
/// identity the ciphertext belongs to, so a valid blob cannot be transplanted
/// into another row, record or recipient.
[[nodiscard]] inline std::vector<uint8_t> aead_aad(
        std::string_view purpose,
        std::initializer_list<std::span<const uint8_t>> context = {}) {
    std::vector<uint8_t> aad;
    const auto put = [&aad](const uint8_t* data, std::size_t size) {
        const auto n = static_cast<uint32_t>(size);
        aad.push_back(static_cast<uint8_t>(n & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
        aad.insert(aad.end(), data, data + size);
    };
    aad.push_back(kEncryptedBlobVersion);
    put(reinterpret_cast<const uint8_t*>(purpose.data()), purpose.size());
    for (const auto& piece : context) {
        put(piece.data(), piece.size());
    }
    return aad;
}

/// The fixed purpose tags. One per construction; never reused across objects.
namespace aead_purpose {
inline constexpr std::string_view kAclPermissions = "acl-permissions";
inline constexpr std::string_view kDdnsAtRest = "ddns-at-rest";
inline constexpr std::string_view kDdnsCredentialTransfer = "ddns-credential-transfer";
inline constexpr std::string_view kCertBundle = "cert-bundle";
inline constexpr std::string_view kKeyWrapping = "key-wrapping";
inline constexpr std::string_view kChildKey = "child-key";
inline constexpr std::string_view kDelegationWrappingKey = "delegation-wrapping-key";
inline constexpr std::string_view kSealedBlob = "sealed-blob";
inline constexpr std::string_view kStorageDataKey = "storage-data-key";
inline constexpr std::string_view kStorageShareAtRest = "storage-share-at-rest";
inline constexpr std::string_view kStorageFileAtRest = "storage-file-at-rest";
inline constexpr std::string_view kVoteKeyAtRest = "vote-key-at-rest";
}  // namespace aead_purpose

// --- HKDF ---
static constexpr std::size_t kHkdfSaltSize = 32;
using HkdfSalt = std::array<uint8_t, kHkdfSaltSize>;

// --- Hashing ---
static constexpr std::size_t kHash256Size = 32;
using Hash256 = std::array<uint8_t, kHash256Size>;

// --- Base64 helpers ---
[[nodiscard]] std::string to_base64(std::span<const uint8_t> data);
[[nodiscard]] std::vector<uint8_t> from_base64(std::string_view encoded);

// from_base64 accepts standard AND url-safe-unpadded, to_base64 emits only
// standard — so one key has several textual forms. Any string used as a security
// identity (revocation, lookup, ACL principal) MUST be canonicalized first,
// otherwise an exact-string compare is bypassed by re-encoding the same key.
// Returns empty on undecodable input; strips an optional "ed25519:" prefix.
[[nodiscard]] std::string canonical_key_b64(std::string_view encoded);

// --- Hex helpers ---
[[nodiscard]] std::string to_hex(std::span<const uint8_t> data);
[[nodiscard]] std::vector<uint8_t> from_hex(std::string_view hex);

} // namespace nexus::crypto
