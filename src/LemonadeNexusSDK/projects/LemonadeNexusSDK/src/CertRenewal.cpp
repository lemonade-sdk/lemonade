#include <LemonadeNexusSDK/CertRenewal.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>
#include <vector>

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace lnsdk {

namespace fs = std::filesystem;

namespace {

constexpr const char* kFullchainFile = "fullchain.pem";
constexpr const char* kPrivkeyFile   = "privkey.pem";

uint64_t asn1_to_unix(const ASN1_TIME* when) {
    if (when == nullptr) return 0;
    struct tm t{};
    if (ASN1_TIME_to_tm(when, &t) != 1) return 0;
#if defined(_WIN32)
    const time_t epoch = _mkgmtime(&t);
#else
    const time_t epoch = timegm(&t);
#endif
    return epoch > 0 ? static_cast<uint64_t>(epoch) : 0;
}

BIO* memory_bio(const std::string& pem) {
    if (pem.empty() || pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }
    return BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
}

/// True when `privkey_pem` is the private key of the leaf in `fullchain_pem`.
bool key_matches_cert(const std::string& fullchain_pem, const std::string& privkey_pem) {
    BIO* cert_bio = memory_bio(fullchain_pem);
    X509* cert = cert_bio ? PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr) : nullptr;
    if (cert_bio) BIO_free(cert_bio);
    BIO* key_bio = memory_bio(privkey_pem);
    EVP_PKEY* key = key_bio ? PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr)
                            : nullptr;
    if (key_bio) BIO_free(key_bio);
    const bool ok = cert != nullptr && key != nullptr && X509_check_private_key(cert, key) == 1;
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
    return ok;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::path temp_path_for(const fs::path& path) {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string suffix;
    for (int i = 0; i < 4; ++i) {
        const auto r = rd();
        for (int b = 0; b < 4; ++b) suffix.push_back(kHex[(r >> (b * 8 + 4)) & 0xF]);
    }
    return path.parent_path() / (path.filename().string() + ".tmp-" + suffix);
}

/// Write `content` to a new file at `path` with `mode` (POSIX) and flush it
/// to stable storage. Fails if `path` exists.
bool write_new_file(const fs::path& path, const std::string& content, int mode,
                    std::string& error) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          static_cast<mode_t>(mode));
    if (fd < 0) {
        error = "cannot create " + path.string();
        return false;
    }
    const char* data = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            error = "cannot write " + path.string();
            return false;
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        error = "cannot flush " + path.string();
        return false;
    }
    if (::close(fd) != 0) {
        error = "cannot close " + path.string();
        return false;
    }
    return true;
#else
    (void)mode;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return true;
#endif
}

void sync_directory([[maybe_unused]] const fs::path& dir) {
#ifndef _WIN32
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
#endif
}

} // namespace

std::optional<CertValidity> read_cert_validity(const std::string& pem) {
    BIO* bio = memory_bio(pem);
    if (!bio) return std::nullopt;
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) return std::nullopt;
    CertValidity validity{
        .not_before = asn1_to_unix(X509_get0_notBefore(cert)),
        .not_after  = asn1_to_unix(X509_get0_notAfter(cert)),
    };
    X509_free(cert);
    if (validity.not_before == 0 || validity.not_after == 0) return std::nullopt;
    return validity;
}

bool cert_renewal_due(const CertValidity& validity, uint64_t now) {
    if (validity.not_before == 0 || validity.not_after <= validity.not_before ||
        now >= validity.not_after) {
        return true;
    }
    const uint64_t lifetime  = validity.not_after - validity.not_before;
    const uint64_t remaining = validity.not_after - now;
    return remaining < lifetime / 3;
}

bool write_cert_files(const fs::path& dir, const std::string& fullchain_pem,
                      const std::string& privkey_pem, std::string& error) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) {
            error = "cannot create " + dir.string();
            return false;
        }
#ifndef _WIN32
        (void)::chmod(dir.c_str(), 0700);
#endif
    }

    const auto key_path   = dir / kPrivkeyFile;
    const auto chain_path = dir / kFullchainFile;
    const auto key_tmp    = temp_path_for(key_path);
    const auto chain_tmp  = temp_path_for(chain_path);

    // Stage both files before replacing either, so a failed write changes
    // nothing.
    const auto discard = [&] {
        fs::remove(key_tmp, ec);
        fs::remove(chain_tmp, ec);
    };
    if (!write_new_file(key_tmp, privkey_pem, 0600, error) ||
        !write_new_file(chain_tmp, fullchain_pem, 0644, error)) {
        discard();
        return false;
    }

    fs::rename(key_tmp, key_path, ec);
    if (ec) {
        error = "cannot replace " + key_path.string();
        discard();
        return false;
    }
    fs::rename(chain_tmp, chain_path, ec);
    if (ec) {
        error = "cannot replace " + chain_path.string();
        discard();
        return false;
    }
    sync_directory(dir);
    return true;
}

// ---------------------------------------------------------------------------
// CertRenewer
// ---------------------------------------------------------------------------

uint64_t CertRenewer::backoff_seconds(uint32_t failures) {
    if (failures == 0) return 0;
    uint64_t delay = kBackoffBaseSeconds;
    for (uint32_t i = 1; i < failures && delay < kBackoffMaxSeconds; ++i) delay *= 2;
    return delay < kBackoffMaxSeconds ? delay : kBackoffMaxSeconds;
}

void CertRenewer::set_callback(RenewedCallback cb) {
    std::lock_guard lock(callback_mtx_);
    callback_ = std::move(cb);
}

void CertRenewer::notify(const CertRenewedEvent& event) {
    std::lock_guard lock(callback_mtx_);
    if (!callback_) return;
    try {
        callback_(event);
    } catch (const std::exception& e) {
        spdlog::warn("[CertRenewer] renewed callback threw: {}", e.what());
    } catch (...) {
        spdlog::warn("[CertRenewer] renewed callback threw");
    }
}

void CertRenewer::record_outcome(const std::string& target, const fs::path& dir,
                                 bool failed, uint64_t now) {
    std::lock_guard lock(state_mtx_);
    auto& entry = managed_[target];
    entry.dir = dir;
    if (failed) {
        if (entry.failures < 32) ++entry.failures;
        entry.next_attempt = now + backoff_seconds(entry.failures);
    } else {
        entry.failures = 0;
        entry.next_attempt = 0;
    }
}

CertRenewalResult CertRenewer::renew_locked(const std::string& target, const fs::path& dir,
                                            uint64_t now, const Fetch& fetch) {
    CertRenewalResult result;
    result.fullchain_path = (dir / kFullchainFile).string();
    result.privkey_path   = (dir / kPrivkeyFile).string();

    std::error_code ec;
    std::optional<CertValidity> current;
    if (fs::exists(dir / kPrivkeyFile, ec)) {
        current = read_cert_validity(read_file(dir / kFullchainFile));
    }
    if (current) result.expires_at = current->not_after;
    if (current && !cert_renewal_due(*current, now)) {
        result.status = CertRenewalStatus::NotDue;
        return result;
    }

    if (!fetch) {
        result.error = "no certificate source";
        return result;
    }
    auto fetched = fetch(target);
    result.http_status = fetched.http_status;
    if (!fetched.ok) {
        result.error = fetched.error.empty() ? "certificate request failed" : fetched.error;
        return result;
    }
    result.domain = fetched.value.domain;

    const auto validity = read_cert_validity(fetched.value.fullchain_pem);
    if (!validity) {
        result.error = "issued certificate does not parse";
        return result;
    }
    if (now >= validity->not_after) {
        result.error = "issued certificate has expired";
        return result;
    }
    if (current && validity->not_after <= current->not_after) {
        result.error = "server returned no newer certificate";
        return result;
    }
    if (!key_matches_cert(fetched.value.fullchain_pem, fetched.value.privkey_pem)) {
        result.error = "issued key does not match the certificate";
        return result;
    }

    std::string write_error;
    const bool written = write_cert_files(dir, fetched.value.fullchain_pem,
                                          fetched.value.privkey_pem, write_error);
    // The decrypted key is no longer needed in this copy.
    sodium_memzero(fetched.value.privkey_pem.data(), fetched.value.privkey_pem.size());
    if (!written) {
        result.error = write_error;
        return result;
    }

    result.status     = CertRenewalStatus::Renewed;
    result.expires_at = validity->not_after;
    return result;
}

CertRenewalResult CertRenewer::renew_if_needed(const std::string& target, const fs::path& dir,
                                               uint64_t now, const Fetch& fetch) {
    CertRenewalResult result;
    {
        std::lock_guard lock(renew_mtx_);
        result = renew_locked(target, dir, now, fetch);
        // A renewal that is still due (the server could not renew either)
        // backs off like a failure, so the loop does not ask every tick.
        const bool failed =
            result.status == CertRenewalStatus::Failed ||
            (result.status == CertRenewalStatus::Renewed &&
             cert_renewal_due(read_cert_validity(read_file(result.fullchain_path))
                                  .value_or(CertValidity{}),
                              now));
        record_outcome(target, dir, failed, now);
    }
    if (result.status == CertRenewalStatus::Renewed) {
        spdlog::info("[CertRenewer] renewed {} (expires_at {})", result.domain, result.expires_at);
        notify(CertRenewedEvent{
            .target         = target,
            .domain         = result.domain,
            .fullchain_path = result.fullchain_path,
            .privkey_path   = result.privkey_path,
            .expires_at     = result.expires_at,
        });
    } else if (result.status == CertRenewalStatus::Failed) {
        spdlog::warn("[CertRenewer] renewal of {} failed: {}", target, result.error);
    }
    return result;
}

bool CertRenewer::renewal_pending(uint64_t now) const {
    std::map<std::string, Managed> snapshot;
    {
        std::lock_guard lock(state_mtx_);
        snapshot = managed_;
    }
    for (const auto& [target, entry] : snapshot) {
        if (entry.next_attempt > now) continue;
        std::error_code ec;
        if (!fs::exists(entry.dir / kPrivkeyFile, ec)) return true;
        const auto validity = read_cert_validity(read_file(entry.dir / kFullchainFile));
        if (!validity || cert_renewal_due(*validity, now)) return true;
    }
    return false;
}

void CertRenewer::renew_managed(uint64_t now, const Fetch& fetch) {
    std::map<std::string, Managed> snapshot;
    {
        std::lock_guard lock(state_mtx_);
        snapshot = managed_;
    }
    for (const auto& [target, entry] : snapshot) {
        if (entry.next_attempt > now) continue;
        (void)renew_if_needed(target, entry.dir, now, fetch);
    }
}

} // namespace lnsdk
