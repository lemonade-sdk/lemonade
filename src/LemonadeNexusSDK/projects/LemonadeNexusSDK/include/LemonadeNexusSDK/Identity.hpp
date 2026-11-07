#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lnsdk {

/// Ed25519 key sizes.
inline constexpr std::size_t kEd25519PublicKeySize  = 32;
inline constexpr std::size_t kEd25519PrivateKeySize = 64;
inline constexpr std::size_t kEd25519SignatureSize  = 64;

using Ed25519PublicKey  = std::array<uint8_t, kEd25519PublicKeySize>;
using Ed25519PrivateKey = std::array<uint8_t, kEd25519PrivateKeySize>;

/// Ed25519 seed size. The 64-byte secret key is seed || public key, so the
/// seed alone reproduces the keypair.
inline constexpr std::size_t kEd25519SeedSize = 32;

/// Manages an Ed25519 keypair for client identity.
/// Uses libsodium directly — no server dependencies.
///
/// The private key is wiped when an Identity is destroyed or moved from.
/// Copies hold their own key bytes and wipe them separately; prefer moving
/// or passing by reference so the key lives in as few places as possible.
class Identity {
public:
    Identity() = default;
    ~Identity();

    Identity(const Identity&) = default;
    Identity& operator=(const Identity&) = default;

    /// Takes the keypair and leaves `other` invalid with its key wiped.
    Identity(Identity&& other) noexcept;
    Identity& operator=(Identity&& other) noexcept;

    /// Generate a new random Ed25519 keypair.
    void generate();

    /// Create an Ed25519 keypair from a 32-byte seed.
    /// This is used when the seed is derived from PBKDF2(username, password).
    void from_seed(std::span<const uint8_t> seed);

    /// Derive a 32-byte Ed25519 seed from username + password using PBKDF2-SHA256.
    /// Uses 100,000 iterations with salt "lemonade-nexus:{username}".
    /// Returns the 32-byte seed, or empty vector on failure.
    [[nodiscard]] static std::vector<uint8_t> derive_seed(const std::string& username,
                                                           const std::string& password);

    /// Check if an identity has been loaded or generated.
    [[nodiscard]] bool is_valid() const noexcept;

    /// Load keypair from a JSON file.
    /// Format: {"public_key":"<base64>","private_key":"<base64>"}
    [[nodiscard]] bool load(const std::filesystem::path& path);

    /// Load keypair from JSON text in the format that load() reads. The
    /// caller owns `text` and should wipe it afterwards. Returns false and
    /// leaves this identity unchanged when the text is malformed.
    [[nodiscard]] bool from_json_text(std::string_view text);

    /// Save keypair to a JSON file, in the format that load() reads.
    ///
    /// Creates the parent directories when needed. Writes a new file beside
    /// `path` and renames it over `path`, so a reader sees either the old file
    /// or the complete new one. On POSIX the new file is created with mode
    /// 0600 and is never readable by other users, and a replaced file of any
    /// mode becomes 0600. On Windows the file takes the access control list
    /// of its directory. Returns false when there is no valid keypair or the
    /// write fails; the existing file is then unchanged.
    [[nodiscard]] bool save(const std::filesystem::path& path) const;

    /// Sign a message with the private key.
    [[nodiscard]] std::vector<uint8_t> sign(std::span<const uint8_t> message) const;

    /// Verify a signature against a public key.
    [[nodiscard]] static bool verify(const Ed25519PublicKey& pubkey,
                                     std::span<const uint8_t> message,
                                     std::span<const uint8_t> signature);

    /// Return the public key in "ed25519:<base64>" format used by the server.
    [[nodiscard]] std::string pubkey_string() const;

    /// Return the raw public key.
    [[nodiscard]] const Ed25519PublicKey& public_key() const noexcept { return pubkey_; }

    /// Return the raw private key.
    [[nodiscard]] const Ed25519PrivateKey& private_key() const noexcept { return privkey_; }

    /// Return the 32-byte seed: the first half of the private key. Secret.
    [[nodiscard]] std::span<const uint8_t, kEd25519SeedSize> seed() const noexcept {
        return std::span<const uint8_t, kEd25519SeedSize>(privkey_.data(), kEd25519SeedSize);
    }

    /// Encode bytes as base64 (URL-safe, no padding).
    [[nodiscard]] static std::string to_base64(std::span<const uint8_t> data);

    /// Decode base64 to bytes.
    [[nodiscard]] static std::vector<uint8_t> from_base64(std::string_view encoded);

private:
    Ed25519PublicKey  pubkey_{};
    Ed25519PrivateKey privkey_{};
    bool              valid_{false};
};

} // namespace lnsdk
