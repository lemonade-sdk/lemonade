#pragma once

/// Account group-key envelopes for zero-knowledge multi-device sharing.
///
/// A Cluster (account) has one symmetric group key that encrypts its data. To
/// give a device access, the group key is *sealed to that device's public key*
/// and stored on the server as an opaque envelope the server cannot open. Only
/// the target device's private key can unseal it.
///
/// Sealing uses a libsodium sealed box (ephemeral X25519 + XSalsa20-Poly1305):
/// the recipient's Ed25519 identity key is converted to X25519, an ephemeral
/// keypair is generated per envelope, and the box is self-contained (the
/// ephemeral public key is embedded), so there is no shared nonce to manage.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace lnsdk {

/// One device's sealed copy of the account group key. `wrapped_key` is the full
/// sealed box; `ephemeral_pubkey` is its embedded ephemeral X25519 key, surfaced
/// for storage/inspection (unwrap only needs `wrapped_key`). All base64.
struct GroupKeyEnvelope {
    std::string ephemeral_pubkey;
    std::string wrapped_key;
};

class GroupKey {
public:
    /// A fresh random 32-byte account group key (base64), or "" on failure.
    [[nodiscard]] static std::string generate();

    /// Seal `group_key_b64` to a recipient Ed25519 public key ("ed25519:<b64>"
    /// or raw base64). Returns nullopt on malformed input.
    [[nodiscard]] static std::optional<GroupKeyEnvelope> wrap(
        const std::string& recipient_ed25519_pubkey, const std::string& group_key_b64);

    /// Open an envelope with our 64-byte Ed25519 private key. Returns the group
    /// key (base64), or nullopt if the envelope isn't addressed to us / is corrupt.
    [[nodiscard]] static std::optional<std::string> unwrap(
        std::span<const uint8_t> our_ed25519_privkey, const GroupKeyEnvelope& env);
};

} // namespace lnsdk
