#pragma once

/// Single-use sealed transfer of a device identity between two processes
/// on the same machine, for example from the user's session to a daemon.
///
/// The receiver creates an ephemeral X25519 keypair and publishes a request
/// `{alg, pubkey, nonce, expires_at}`. The sender seals `nonce || seed` to
/// that key with a libsodium sealed box and returns the envelope. The
/// receiver opens the envelope once: the first open, successful or not,
/// wipes the ephemeral secret, so a captured envelope cannot be replayed and
/// an envelope for another request fails. The channel between the two
/// carries ciphertext only.

#include <LemonadeNexusSDK/Identity.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lnsdk {

class IdentityHandoff {
public:
    static constexpr std::string_view kAlg = "x25519-sealedbox-v1";
    static constexpr std::chrono::seconds kLifetime{60};

    enum class OpenResult {
        Ok,
        AlreadyUsed,  ///< The ephemeral key was consumed by an earlier open.
        Expired,      ///< `expires_at` has passed.
        Malformed,    ///< The envelope is not valid JSON or lacks fields.
        Rejected,     ///< Wrong `alg`, or the envelope names another request.
        Unsealable,   ///< The sealed box does not open for this key and nonce.
    };

    /// Receiver side: creates the ephemeral keypair and a request that stays
    /// valid for `lifetime`. Products use the default; tests shorten it.
    explicit IdentityHandoff(std::chrono::seconds lifetime = kLifetime);
    ~IdentityHandoff();

    IdentityHandoff(const IdentityHandoff&) = delete;
    IdentityHandoff& operator=(const IdentityHandoff&) = delete;
    IdentityHandoff(IdentityHandoff&&) noexcept;
    IdentityHandoff& operator=(IdentityHandoff&&) noexcept;

    /// False when key generation failed; then request_json() is empty.
    [[nodiscard]] bool valid() const noexcept;

    /// True after the first open().
    [[nodiscard]] bool used() const noexcept;

    /// The request for the sender: `{alg, pubkey, nonce, expires_at}` with
    /// base64 values and `expires_at` in Unix seconds. Carries no secret.
    [[nodiscard]] std::string request_json() const;

    /// Sender side: seal `identity` to a request. Returns the envelope JSON
    /// `{alg, nonce, sealed}`, or nullopt when the request is malformed, its
    /// `alg` is unknown, or `identity` is not valid.
    [[nodiscard]] static std::optional<std::string> seal(const Identity& identity,
                                                         std::string_view request_json);

    /// Receiver side: open an envelope. Consumes the ephemeral secret on the
    /// first call whatever the result. On Ok, `out` holds the identity.
    /// Thread-safe: concurrent calls serialize and at most one can succeed.
    [[nodiscard]] OpenResult open(std::string_view envelope_json, Identity& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lnsdk
