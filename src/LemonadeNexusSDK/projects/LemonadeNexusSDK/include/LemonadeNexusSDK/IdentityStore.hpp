#pragma once

/// Per-user device identity protected by the OS keystore.
///
/// The store keeps one Ed25519 identity for the effective OS user. The
/// 32-byte seed is wrapped with a random 32-byte key-encryption key (KEK)
/// using libsodium `crypto_secretbox`. The KEK lives in the OS keystore:
/// the login keychain on macOS, a user-scope DPAPI blob on Windows, and the
/// Secret Service through libsecret on Linux. The identity file holds the
/// public key, the wrapped seed, and a reference to the KEK. It never holds
/// the seed or the KEK, so reading the file alone yields nothing secret.
///
/// There is no fallback: when the keystore is unavailable the store reports
/// `Unsupported` and writes no file.

#include <LemonadeNexusSDK/Identity.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nexus::crypto {
class SecureBuffer;
}

namespace lnsdk {

/// Outcome of one keystore operation.
enum class KeystoreStatus {
    Ok,
    NotFound,     ///< No item for the given reference.
    Denied,       ///< The keystore is locked or refused access.
    Unavailable,  ///< No keystore on this system or in this session.
    Failed,       ///< Any other failure.
};

/// Holds the KEK of a stored identity. One implementation per platform; tests
/// inject an in-memory implementation. Implementations must not log or
/// persist the KEK outside the keystore.
class KeystoreBackend {
public:
    virtual ~KeystoreBackend() = default;

    /// Backend name as reported in status: "keychain", "dpapi", "libsecret".
    [[nodiscard]] virtual std::string name() const = 0;

    /// True when the keystore can be used in this process. A false result
    /// means every other call returns `Unavailable`.
    [[nodiscard]] virtual bool available() const = 0;

    /// Store `kek` for the identity whose public key is `pubkey_b64`. On `Ok`,
    /// sets `kek_ref` to the JSON value that the identity file keeps to find
    /// the KEK again (an item name, or the protected blob itself for DPAPI).
    /// Replaces an item left behind for the same public key.
    virtual KeystoreStatus put(std::span<const uint8_t> kek, const std::string& pubkey_b64,
                               nlohmann::json& kek_ref) = 0;

    /// Read the KEK that `kek_ref` names into `kek`.
    virtual KeystoreStatus get(const nlohmann::json& kek_ref, nexus::crypto::SecureBuffer& kek) = 0;

    /// Remove the KEK that `kek_ref` names. A missing item is `NotFound`.
    virtual KeystoreStatus remove(const nlohmann::json& kek_ref) = 0;
};

/// Result of a store operation.
enum class StoreResult {
    Ok,
    NotFound,     ///< No identity is stored.
    Denied,       ///< The keystore is locked or refused access.
    Unsupported,  ///< No usable keystore, or the file was wrapped for another one.
    Exists,       ///< An identity is already stored; create and import refuse.
    Invalid,      ///< The file or the wrapped seed failed verification.
    Failed,       ///< Input/output or internal failure.
};

/// What `IdentityStore::status()` reports. Never carries secret material.
struct IdentityStoreStatus {
    bool present{false};        ///< An identity file exists and parses.
    std::string backend;        ///< Backend name, or "none" when unavailable.
    bool unlocked{false};       ///< The stored identity loads right now.
    std::string public_key;     ///< "ed25519:<base64>" when present, else empty.
    std::filesystem::path dir;  ///< The directory of the store.
};

class IdentityStore {
public:
    static constexpr std::string_view kFileName = "identity.v2.json";
    static constexpr int kFormatVersion = 2;

    /// A store in `dir` whose KEKs live in `backend`. `backend` must not be
    /// null. Mainly for tests; products use `for_user()`.
    IdentityStore(std::filesystem::path dir, std::shared_ptr<KeystoreBackend> backend);

    /// The store of the effective OS user with the platform keystore. The
    /// directory comes from `user_dir()`. When no directory can be resolved
    /// the store is unusable and every operation returns `Failed`.
    [[nodiscard]] static IdentityStore for_user();

    /// Canonical per-user directory:
    /// macOS `~/Library/Application Support/LemonadeNexus/identity`,
    /// Windows `%LOCALAPPDATA%\LemonadeNexus\identity`,
    /// Linux `${XDG_DATA_HOME:-~/.local/share}/lemonade-nexus/identity`.
    /// The home directory is that of the effective user (`getpwuid(geteuid())`
    /// or the known folder of the process token), never `SUDO_USER`. `HOME`
    /// is used only when the account database has no directory for the user.
    /// Returns nullopt when nothing can be resolved. Does not create it.
    [[nodiscard]] static std::optional<std::filesystem::path> user_dir();

    /// The keystore backend of this platform. Never null; `available()` tells
    /// whether it can be used.
    [[nodiscard]] static std::shared_ptr<KeystoreBackend> platform_backend();

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
    [[nodiscard]] std::filesystem::path file_path() const { return dir_ / kFileName; }
    [[nodiscard]] KeystoreBackend& backend() const noexcept { return *backend_; }

    /// Report what is stored without returning secret material. Reading the
    /// KEK to compute `unlocked` may ask the OS keystore for access.
    [[nodiscard]] IdentityStoreStatus status() const;

    /// Load the stored identity. `NotFound` when no file exists, `Denied` when
    /// the keystore refuses, `Unsupported` when no keystore can serve the
    /// file, `Invalid` when the file or the ciphertext fails verification or
    /// the keystore no longer has the KEK (remove() and create() again).
    [[nodiscard]] StoreResult load(Identity& out) const;

    /// Create and store a new identity. `Exists` when a file is present. No
    /// file is written when the keystore refuses the KEK.
    [[nodiscard]] StoreResult create(Identity& out);

    /// Store an existing identity, for one-time migration. `Exists` when a
    /// file is present; `Invalid` when `identity` is not valid.
    [[nodiscard]] StoreResult import(const Identity& identity);

    /// Remove the keystore item, then the file. `NotFound` when no file
    /// exists. The file stays when the keystore item cannot be removed for a
    /// reason other than being absent, so the caller can retry.
    [[nodiscard]] StoreResult remove();

    /// Makes `for_user()` return a store with this directory and backend
    /// while the object lives. For tests that must not touch the real user
    /// store. Not exported from the shared library. Not for products.
    class ScopedOverride {
    public:
        ScopedOverride(std::filesystem::path dir, std::shared_ptr<KeystoreBackend> backend);
        ~ScopedOverride();
        ScopedOverride(const ScopedOverride&) = delete;
        ScopedOverride& operator=(const ScopedOverride&) = delete;
    };

private:
    struct Record;
    StoreResult read_record(Record& out) const;
    StoreResult store_seed(std::span<const uint8_t> seed, const Identity& identity);

    std::filesystem::path dir_;
    std::shared_ptr<KeystoreBackend> backend_;
};

namespace detail {
/// Defined by the platform source file (IdentityStore_<os>.cpp).
std::shared_ptr<KeystoreBackend> make_platform_backend();
}  // namespace detail

}  // namespace lnsdk
