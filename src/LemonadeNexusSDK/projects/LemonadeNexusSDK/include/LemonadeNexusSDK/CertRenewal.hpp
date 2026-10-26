#pragma once

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace lnsdk {

/// Certificate target of the caller's group loopback wildcard certificate
/// ("*.<group-label>.lo.<base_domain>"). Every other target is a node
/// hostname, which names the device certificate "<hostname>.capi.<base>".
inline constexpr std::string_view kGroupLoopbackCertTarget = "*";

/// Validity period of the leaf certificate, Unix seconds.
struct CertValidity {
    uint64_t not_before{0};
    uint64_t not_after{0};
};

/// The validity period of the first certificate in a PEM chain, or nullopt
/// when it does not parse.
[[nodiscard]] std::optional<CertValidity> read_cert_validity(const std::string& pem);

/// True when the certificate must be requested again at `now`: less than one
/// third of its lifetime remains, it has expired, or its period is invalid.
/// The server applies the same rule before it issues again.
[[nodiscard]] bool cert_renewal_due(const CertValidity& validity, uint64_t now);

/// Write `fullchain.pem` and `privkey.pem` into `dir`, each through a
/// temporary file in the same directory that is flushed and then renamed
/// over the old file. The key file is created with mode 0600 (POSIX). `dir`
/// is created (mode 0700 on POSIX) when missing. The key file is replaced
/// first. On failure the old files stay and `error` describes the step.
[[nodiscard]] bool write_cert_files(const std::filesystem::path& dir,
                                    const std::string& fullchain_pem,
                                    const std::string& privkey_pem,
                                    std::string& error);

/// Outcome of one renewal check.
enum class CertRenewalStatus {
    Renewed,   ///< new files written and the callback fired
    NotDue,    ///< the stored certificate is not due; nothing requested
    Failed,    ///< request, check, or write failed; the old files stay
};

struct CertRenewalResult {
    CertRenewalStatus status{CertRenewalStatus::Failed};
    std::string domain;          ///< certificate name, when known
    uint64_t    expires_at{0};   ///< Not After of the certificate now on disk
    std::string fullchain_path;
    std::string privkey_path;
    int         http_status{0};  ///< HTTP status of a failed request, else 0
    std::string error;
};

/// Passed to the renewed callback after new files are in place.
struct CertRenewedEvent {
    std::string target;          ///< hostname, or kGroupLoopbackCertTarget
    std::string domain;
    std::string fullchain_path;
    std::string privkey_path;
    uint64_t    expires_at{0};
};

/// Keeps device and group loopback certificates on disk current.
///
/// A target is managed after its first renew_if_needed() call. The renewer
/// requests a new certificate (through `fetch`) only when the stored one is
/// missing or cert_renewal_due(). A failed attempt keeps the old files and
/// delays the next background attempt with exponential backoff (60 s,
/// doubled per failure, at most 6 h). Thread-safe. Renewals are serialized.
class CertRenewer {
public:
    /// Requests and decrypts one certificate for a target. Called without
    /// the renewer's state lock; it may block on the network.
    using Fetch = std::function<Result<DecryptedCert>(const std::string& target)>;
    /// Called once per renewal, after both files are replaced, on the thread
    /// that ran the renewal. It must not call set_callback() (deadlock).
    using RenewedCallback = std::function<void(const CertRenewedEvent&)>;

    static constexpr uint64_t kBackoffBaseSeconds = 60;
    static constexpr uint64_t kBackoffMaxSeconds  = 6 * 3600;

    /// Replace the callback (empty clears it). When this returns, the old
    /// callback is not running and is not called again.
    void set_callback(RenewedCallback cb);

    /// Check `target`'s files in `dir` at `now` (Unix seconds) and renew them
    /// when due. Ignores the backoff delay. Registers the target for
    /// renew_managed(); a later call with another `dir` moves it.
    CertRenewalResult renew_if_needed(const std::string& target,
                                      const std::filesystem::path& dir,
                                      uint64_t now, const Fetch& fetch);

    /// True when some managed target is due at `now` and outside its backoff
    /// delay. Reads only local files.
    [[nodiscard]] bool renewal_pending(uint64_t now) const;

    /// Renew every managed target that is due at `now` and outside its
    /// backoff delay.
    void renew_managed(uint64_t now, const Fetch& fetch);

    /// Seconds of the backoff delay after `failures` consecutive failures.
    [[nodiscard]] static uint64_t backoff_seconds(uint32_t failures);

private:
    struct Managed {
        std::filesystem::path dir;
        uint32_t failures{0};
        uint64_t next_attempt{0};   ///< Unix seconds; 0 = no delay
    };

    CertRenewalResult renew_locked(const std::string& target,
                                   const std::filesystem::path& dir,
                                   uint64_t now, const Fetch& fetch);
    void record_outcome(const std::string& target, const std::filesystem::path& dir,
                        bool failed, uint64_t now);
    void notify(const CertRenewedEvent& event);

    mutable std::mutex       state_mtx_;    // managed_
    std::map<std::string, Managed> managed_;
    std::mutex               renew_mtx_;    // serializes renewals
    std::mutex               callback_mtx_; // callback_ and its invocation
    RenewedCallback          callback_;
};

} // namespace lnsdk
