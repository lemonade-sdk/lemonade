#include <LemonadeNexusSDK/Identity.hpp>

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>

namespace lnsdk {

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
    if (sodium_init() < 0) {
        spdlog::error("[Identity] libsodium initialization failed");
        return false;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            spdlog::debug("[Identity] keypair file not found: {}", path.string());
            return false;
        }

        auto j = nlohmann::json::parse(file);
        auto pub_bytes  = from_base64(j.value("public_key", ""));
        auto priv_bytes = from_base64(j.value("private_key", ""));

        if (pub_bytes.size() != kEd25519PublicKeySize ||
            priv_bytes.size() != kEd25519PrivateKeySize) {
            spdlog::warn("[Identity] stored keypair has wrong size");
            return false;
        }

        std::memcpy(pubkey_.data(), pub_bytes.data(), kEd25519PublicKeySize);
        std::memcpy(privkey_.data(), priv_bytes.data(), kEd25519PrivateKeySize);
        valid_ = true;

        spdlog::info("[Identity] loaded keypair from {}", path.string());
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("[Identity] failed to load keypair: {}", e.what());
        return false;
    }
}

bool Identity::save(const std::filesystem::path& path) const {
    if (!valid_) {
        spdlog::warn("[Identity] cannot save: no valid keypair");
        return false;
    }

    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        nlohmann::json j;
        j["public_key"]  = to_base64(pubkey_);
        j["private_key"] = to_base64(privkey_);

        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("[Identity] cannot open {} for writing", path.string());
            return false;
        }
        file << j.dump(2);
        spdlog::info("[Identity] saved keypair to {}", path.string());
        return true;

    } catch (const std::exception& e) {
        spdlog::error("[Identity] failed to save keypair: {}", e.what());
        return false;
    }
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
