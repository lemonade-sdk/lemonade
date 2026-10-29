#include <LemonadeNexusSDK/Identity.hpp>

#include "OwnerOnlyFile.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lnsdk {

Identity::~Identity() {
    sodium_memzero(privkey_.data(), privkey_.size());
}

Identity::Identity(Identity&& other) noexcept
    : pubkey_(other.pubkey_), privkey_(other.privkey_), valid_(other.valid_) {
    sodium_memzero(other.privkey_.data(), other.privkey_.size());
    other.valid_ = false;
}

Identity& Identity::operator=(Identity&& other) noexcept {
    if (this != &other) {
        pubkey_  = other.pubkey_;
        privkey_ = other.privkey_;
        valid_   = other.valid_;
        sodium_memzero(other.privkey_.data(), other.privkey_.size());
        other.valid_ = false;
    }
    return *this;
}

void Identity::generate() {
    if (sodium_init() < 0) {
        spdlog::error("[Identity] libsodium initialization failed");
        return;
    }
    crypto_sign_ed25519_keypair(pubkey_.data(), privkey_.data());
    valid_ = true;
    spdlog::info("[Identity] generated new Ed25519 keypair");
}

void Identity::from_seed(std::span<const uint8_t> seed) {
    if (seed.size() != 32) {
        spdlog::error("[Identity] from_seed requires exactly 32 bytes, got {}", seed.size());
        return;
    }
    if (sodium_init() < 0) {
        spdlog::error("[Identity] libsodium initialization failed");
        return;
    }
    // libsodium: derive keypair from 32-byte seed
    crypto_sign_ed25519_seed_keypair(pubkey_.data(), privkey_.data(), seed.data());
    valid_ = true;
    spdlog::info("[Identity] created Ed25519 keypair from seed");
}

std::vector<uint8_t> Identity::derive_seed(const std::string& username,
                                            const std::string& password) {
    if (sodium_init() < 0) {
        spdlog::error("[Identity] libsodium initialization failed");
        return {};
    }

    // Match the Swift client's PBKDF2 parameters exactly:
    // Salt: "lemonade-nexus:{username}"
    // Iterations: 100,000
    // Algorithm: HMAC-SHA256
    // Output: 32 bytes
    std::string salt = "lemonade-nexus:" + username;

    std::vector<uint8_t> derived(32);

    // libsodium's pwhash is not PBKDF2, so we use crypto_pwhash with
    // Argon2id. But wait — the Swift client uses PBKDF2-HMAC-SHA256 via
    // CommonCrypto's CCKeyDerivationPBKDF. We need to match that exactly.
    //
    // libsodium doesn't provide PBKDF2 directly. We'll implement it using
    // crypto_auth_hmacsha256 (HMAC-SHA256).

    // PBKDF2-HMAC-SHA256 implementation
    // Per RFC 2898:
    // DK = T1 || T2 || ... || Tdklen/hlen
    // Ti = F(Password, Salt, c, i)
    // F(Password, Salt, c, i) = U1 ^ U2 ^ ... ^ Uc
    // U1 = PRF(Password, Salt || INT(i))
    // Uj = PRF(Password, Uj-1)

    const uint32_t iterations = 100000;
    const size_t h_len = 32; // SHA-256 output

    // We only need one block (dk_len == h_len)
    // Prepare Salt || INT(1) where INT(1) is big-endian 4 bytes
    std::vector<uint8_t> salt_i(salt.begin(), salt.end());
    uint8_t block_idx[4] = {0, 0, 0, 1}; // big-endian 1
    salt_i.insert(salt_i.end(), block_idx, block_idx + 4);

    // U1 = HMAC-SHA256(password, salt || INT(1))
    crypto_auth_hmacsha256_state state;
    std::vector<uint8_t> u_prev(h_len);
    std::vector<uint8_t> result(h_len, 0);

    // U1
    crypto_auth_hmacsha256_init(&state,
                                 reinterpret_cast<const uint8_t*>(password.data()),
                                 password.size());
    crypto_auth_hmacsha256_update(&state, salt_i.data(), salt_i.size());
    crypto_auth_hmacsha256_final(&state, u_prev.data());

    // result = U1
    std::copy(u_prev.begin(), u_prev.end(), result.begin());

    // U2..Uc
    for (uint32_t j = 1; j < iterations; ++j) {
        std::vector<uint8_t> u_next(h_len);
        crypto_auth_hmacsha256_init(&state,
                                     reinterpret_cast<const uint8_t*>(password.data()),
                                     password.size());
        crypto_auth_hmacsha256_update(&state, u_prev.data(), u_prev.size());
        crypto_auth_hmacsha256_final(&state, u_next.data());

        // XOR into result
        for (size_t k = 0; k < h_len; ++k) {
            result[k] ^= u_next[k];
        }
        u_prev = std::move(u_next);
    }

    // Zero sensitive data
    sodium_memzero(u_prev.data(), u_prev.size());

    derived = std::move(result);
    spdlog::info("[Identity] derived 32-byte seed via PBKDF2-SHA256 (100k iterations)");
    return derived;
}

bool Identity::is_valid() const noexcept {
    return valid_;
}

bool Identity::load(const std::filesystem::path& path) {
    std::string text;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            spdlog::debug("[Identity] keypair file not found: {}", path.string());
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    const bool ok = from_json_text(text);
    sodium_memzero(text.data(), text.size());
    if (ok) {
        spdlog::info("[Identity] loaded keypair from {}", path.string());
    }
    return ok;
}

bool Identity::from_json_text(std::string_view text) {
    if (sodium_init() < 0) {
        spdlog::error("[Identity] libsodium initialization failed");
        return false;
    }

    std::vector<uint8_t> priv_bytes;
    bool ok = false;
    try {
        auto j = nlohmann::json::parse(text);
        auto pub_bytes = from_base64(j.value("public_key", ""));
        priv_bytes     = from_base64(j.value("private_key", ""));

        if (pub_bytes.size() != kEd25519PublicKeySize ||
            priv_bytes.size() != kEd25519PrivateKeySize) {
            spdlog::warn("[Identity] stored keypair has wrong size");
        } else {
            std::memcpy(pubkey_.data(), pub_bytes.data(), kEd25519PublicKeySize);
            std::memcpy(privkey_.data(), priv_bytes.data(), kEd25519PrivateKeySize);
            valid_ = true;
            ok = true;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Identity] failed to parse keypair: {}", e.what());
    }
    sodium_memzero(priv_bytes.data(), priv_bytes.size());
    return ok;
}

namespace {

/// Write `text` to a new file at `tmp` that only the owner can read and write.
/// The file is created with that mode, so no other user can open it at any
/// point. Fails when `tmp` already exists.
bool write_owner_only_file(const std::filesystem::path& tmp, const std::string& text) {
#ifdef _WIN32
    int fd = -1;
    if (_wsopen_s(&fd, tmp.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                  _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) {
        return false;
    }
    const char* p = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
        const unsigned chunk = static_cast<unsigned>(
            std::min<std::size_t>(remaining, (std::numeric_limits<int>::max)()));
        const int n = _write(fd, p, chunk);
        if (n <= 0) break;
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    const bool ok = remaining == 0 && _commit(fd) == 0;
    return _close(fd) == 0 && ok;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    // The umask can only remove bits from the creation mode. Set the mode
    // exactly so the result does not depend on the umask.
    bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
    const char* p = text.data();
    std::size_t remaining = text.size();
    while (ok && remaining > 0) {
        const ssize_t n = ::write(fd, p, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    ok = ok && remaining == 0 && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
#endif
}

} // namespace

namespace detail {

bool replace_owner_only_file(const std::filesystem::path& path, const std::string& text) {
    // Write a new owner-only file beside the target, then rename it over
    // the target. The content is never in a file that others can read, and
    // a reader sees either the old file or the complete new one.
    unsigned char suffix[8];
    char suffix_hex[sizeof suffix * 2 + 1];
    randombytes_buf(suffix, sizeof suffix);
    sodium_bin2hex(suffix_hex, sizeof suffix_hex, suffix, sizeof suffix);
    std::filesystem::path tmp = path;
    tmp += std::string(".tmp-") + suffix_hex;

    if (!write_owner_only_file(tmp, text)) {
        spdlog::error("[Identity] cannot write {}", tmp.string());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        spdlog::error("[Identity] cannot replace {}: {}", path.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace detail

bool Identity::save(const std::filesystem::path& path) const {
    if (!valid_) {
        spdlog::warn("[Identity] cannot save: no valid keypair");
        return false;
    }

    std::string text;
    bool ok = false;
    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        nlohmann::json j;
        j["public_key"]  = to_base64(pubkey_);
        j["private_key"] = to_base64(privkey_);
        text = j.dump(2);
        auto& encoded = j["private_key"].get_ref<std::string&>();
        sodium_memzero(encoded.data(), encoded.size());

        ok = detail::replace_owner_only_file(path, text);
        if (ok) {
            spdlog::info("[Identity] saved keypair to {}", path.string());
        }
    } catch (const std::exception& e) {
        spdlog::error("[Identity] failed to save keypair: {}", e.what());
    }

    sodium_memzero(text.data(), text.size());
    return ok;
}

std::vector<uint8_t> Identity::sign(std::span<const uint8_t> message) const {
    if (!valid_) {
        spdlog::error("[Identity] cannot sign: no valid keypair");
        return {};
    }

    std::vector<uint8_t> sig(kEd25519SignatureSize);
    unsigned long long sig_len = 0;
    crypto_sign_ed25519_detached(sig.data(), &sig_len,
                                  message.data(), message.size(),
                                  privkey_.data());
    sig.resize(static_cast<std::size_t>(sig_len));
    return sig;
}

bool Identity::verify(const Ed25519PublicKey& pubkey,
                       std::span<const uint8_t> message,
                       std::span<const uint8_t> signature) {
    if (signature.size() != kEd25519SignatureSize) {
        return false;
    }
    return crypto_sign_ed25519_verify_detached(
        signature.data(), message.data(), message.size(), pubkey.data()) == 0;
}

std::string Identity::pubkey_string() const {
    if (!valid_) return "";
    return "ed25519:" + to_base64(pubkey_);
}

std::string Identity::to_base64(std::span<const uint8_t> data) {
    if (data.empty()) return "";

    const std::size_t max_len = sodium_base64_encoded_len(
        data.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string encoded(max_len, '\0');
    sodium_bin2base64(encoded.data(), max_len,
                       data.data(), data.size(),
                       sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    auto pos = encoded.find('\0');
    if (pos != std::string::npos) {
        encoded.resize(pos);
    }
    return encoded;
}

std::vector<uint8_t> Identity::from_base64(std::string_view encoded) {
    if (encoded.empty()) return {};

    std::vector<uint8_t> decoded(encoded.size());
    std::size_t bin_len = 0;
    if (sodium_base642bin(decoded.data(), decoded.size(),
                           encoded.data(), encoded.size(),
                           nullptr, &bin_len, nullptr,
                           sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        // Try standard base64 as fallback
        if (sodium_base642bin(decoded.data(), decoded.size(),
                               encoded.data(), encoded.size(),
                               nullptr, &bin_len, nullptr,
                               sodium_base64_VARIANT_ORIGINAL) != 0) {
            return {};
        }
    }
    decoded.resize(bin_len);
    return decoded;
}

} // namespace lnsdk
