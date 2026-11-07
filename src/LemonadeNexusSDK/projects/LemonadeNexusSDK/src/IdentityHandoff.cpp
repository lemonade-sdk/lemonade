#include <LemonadeNexusSDK/IdentityHandoff.hpp>

#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using nlohmann::json;
using nexus::crypto::SecureBuffer;

namespace lnsdk {

namespace {

constexpr std::size_t kNonceBytes = 24;
constexpr std::size_t kPlaintextBytes = kNonceBytes + kEd25519SeedSize;
constexpr std::size_t kSealedBytes = crypto_box_SEALBYTES + kPlaintextBytes;

std::int64_t unix_seconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

}  // namespace

struct IdentityHandoff::Impl {
    std::mutex mutex;
    SecureBuffer secret;  ///< X25519 secret key; empty once consumed
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> pubkey{};
    std::vector<uint8_t> nonce;
    std::chrono::system_clock::time_point expires;
    bool valid{false};
    bool used{false};
};

IdentityHandoff::IdentityHandoff(std::chrono::seconds lifetime) : impl_(std::make_unique<Impl>()) {
    if (sodium_init() < 0) return;
    impl_->secret = SecureBuffer(crypto_box_SECRETKEYBYTES);
    if (crypto_box_keypair(impl_->pubkey.data(), impl_->secret.data()) != 0) {
        impl_->secret.clear();
        return;
    }
    impl_->nonce.resize(kNonceBytes);
    randombytes_buf(impl_->nonce.data(), impl_->nonce.size());
    impl_->expires = std::chrono::system_clock::now() + lifetime;
    impl_->valid = true;
}

IdentityHandoff::~IdentityHandoff() = default;
IdentityHandoff::IdentityHandoff(IdentityHandoff&&) noexcept = default;
IdentityHandoff& IdentityHandoff::operator=(IdentityHandoff&&) noexcept = default;

bool IdentityHandoff::valid() const noexcept {
    return impl_ && impl_->valid;
}

bool IdentityHandoff::used() const noexcept {
    if (!impl_) return true;
    std::lock_guard lock(impl_->mutex);
    return impl_->used;
}

std::string IdentityHandoff::request_json() const {
    if (!valid()) return {};
    json j;
    j["alg"] = kAlg;
    j["pubkey"] = Identity::to_base64(impl_->pubkey);
    j["nonce"] = Identity::to_base64(impl_->nonce);
    j["expires_at"] = unix_seconds(impl_->expires);
    return j.dump();
}

std::optional<std::string> IdentityHandoff::seal(const Identity& identity, std::string_view request_json) {
    if (!identity.is_valid() || sodium_init() < 0) return std::nullopt;

    const json request = json::parse(request_json, nullptr, false);
    if (!request.is_object() || request.value("alg", "") != kAlg) return std::nullopt;
    const auto pubkey_it = request.find("pubkey");
    const auto nonce_it = request.find("nonce");
    const auto expires_it = request.find("expires_at");
    if (pubkey_it == request.end() || !pubkey_it->is_string() || nonce_it == request.end() ||
        !nonce_it->is_string() || expires_it == request.end() || !expires_it->is_number_integer()) {
        return std::nullopt;
    }
    const std::vector<uint8_t> pubkey = Identity::from_base64(pubkey_it->get<std::string>());
    const std::vector<uint8_t> nonce = Identity::from_base64(nonce_it->get<std::string>());
    if (pubkey.size() != crypto_box_PUBLICKEYBYTES || nonce.size() != kNonceBytes) return std::nullopt;

    // The nonce travels inside the sealed box, so the envelope binds to the
    // request that asked for it and not only to the recipient key.
    SecureBuffer plaintext(kPlaintextBytes);
    std::memcpy(plaintext.data(), nonce.data(), kNonceBytes);
    std::memcpy(plaintext.data() + kNonceBytes, identity.seed().data(), kEd25519SeedSize);

    std::vector<uint8_t> sealed(kSealedBytes);
    const int rc = crypto_box_seal(sealed.data(), plaintext.data(), plaintext.size(), pubkey.data());
    plaintext.clear();
    if (rc != 0) return std::nullopt;

    json envelope;
    envelope["alg"] = kAlg;
    envelope["nonce"] = nonce_it->get<std::string>();
    envelope["sealed"] = Identity::to_base64(sealed);
    return envelope.dump();
}

IdentityHandoff::OpenResult IdentityHandoff::open(std::string_view envelope_json, Identity& out) {
    if (!impl_) return OpenResult::AlreadyUsed;
    std::lock_guard lock(impl_->mutex);
    if (impl_->used || !impl_->valid) return OpenResult::AlreadyUsed;
    impl_->used = true;

    // The ephemeral secret lives for exactly one attempt.
    SecureBuffer secret = std::move(impl_->secret);
    impl_->secret.clear();

    if (std::chrono::system_clock::now() > impl_->expires) return OpenResult::Expired;

    const json envelope = json::parse(envelope_json, nullptr, false);
    if (!envelope.is_object()) return OpenResult::Malformed;
    const auto alg_it = envelope.find("alg");
    const auto nonce_it = envelope.find("nonce");
    const auto sealed_it = envelope.find("sealed");
    if (alg_it == envelope.end() || !alg_it->is_string() || nonce_it == envelope.end() ||
        !nonce_it->is_string() || sealed_it == envelope.end() || !sealed_it->is_string()) {
        return OpenResult::Malformed;
    }
    if (alg_it->get<std::string>() != kAlg) return OpenResult::Rejected;
    const std::vector<uint8_t> nonce = Identity::from_base64(nonce_it->get<std::string>());
    if (nonce.size() != kNonceBytes) return OpenResult::Malformed;
    if (sodium_memcmp(nonce.data(), impl_->nonce.data(), kNonceBytes) != 0) return OpenResult::Rejected;
    const std::vector<uint8_t> sealed = Identity::from_base64(sealed_it->get<std::string>());
    if (sealed.size() != kSealedBytes) return OpenResult::Malformed;

    SecureBuffer plaintext(kPlaintextBytes);
    if (crypto_box_seal_open(plaintext.data(), sealed.data(), sealed.size(), impl_->pubkey.data(),
                             secret.data()) != 0) {
        return OpenResult::Unsealable;
    }
    secret.clear();
    if (sodium_memcmp(plaintext.data(), impl_->nonce.data(), kNonceBytes) != 0) {
        return OpenResult::Unsealable;
    }

    Identity identity;
    identity.from_seed(std::span<const uint8_t>(plaintext.data() + kNonceBytes, kEd25519SeedSize));
    plaintext.clear();
    if (!identity.is_valid()) return OpenResult::Unsealable;
    out = std::move(identity);
    return OpenResult::Ok;
}

}  // namespace lnsdk
