#include <LemonadeNexusSDK/GroupKey.hpp>

#include <LemonadeNexusSDK/Identity.hpp>  // reuse the shared base64 helpers

#include <sodium.h>

#include <string_view>
#include <vector>

namespace lnsdk {

namespace {
constexpr std::size_t kGroupKeyBytes = 32;

std::string strip_ed_prefix(const std::string& key) {
    constexpr std::string_view prefix = "ed25519:";
    if (key.rfind(prefix, 0) == 0) return key.substr(prefix.size());
    return key;
}
}  // namespace

std::string GroupKey::generate() {
    if (sodium_init() < 0) return "";
    uint8_t key[kGroupKeyBytes];
    randombytes_buf(key, sizeof key);
    auto b64 = Identity::to_base64(std::span<const uint8_t>(key, sizeof key));
    sodium_memzero(key, sizeof key);
    return b64;
}

std::optional<GroupKeyEnvelope> GroupKey::wrap(const std::string& recipient_ed25519_pubkey,
                                               const std::string& group_key_b64) {
    if (sodium_init() < 0) return std::nullopt;

    auto ed_pk = Identity::from_base64(strip_ed_prefix(recipient_ed25519_pubkey));
    if (ed_pk.size() != crypto_sign_PUBLICKEYBYTES) return std::nullopt;

    auto group_key = Identity::from_base64(group_key_b64);
    if (group_key.size() != kGroupKeyBytes) return std::nullopt;

    // Recipient Ed25519 -> X25519 public key.
    uint8_t x_pk[crypto_scalarmult_curve25519_BYTES];
    if (crypto_sign_ed25519_pk_to_curve25519(x_pk, ed_pk.data()) != 0) {
        sodium_memzero(group_key.data(), group_key.size());
        return std::nullopt;
    }

    std::vector<uint8_t> sealed(crypto_box_SEALBYTES + group_key.size());
    int rc = crypto_box_seal(sealed.data(), group_key.data(), group_key.size(), x_pk);
    sodium_memzero(group_key.data(), group_key.size());
    if (rc != 0) return std::nullopt;

    GroupKeyEnvelope env;
    env.wrapped_key = Identity::to_base64(sealed);
    // The sealed box begins with the ephemeral X25519 public key.
    env.ephemeral_pubkey =
        Identity::to_base64(std::span<const uint8_t>(sealed.data(), crypto_box_PUBLICKEYBYTES));
    return env;
}

std::optional<std::string> GroupKey::unwrap(std::span<const uint8_t> our_ed25519_privkey,
                                            const GroupKeyEnvelope& env) {
    if (sodium_init() < 0) return std::nullopt;
    if (our_ed25519_privkey.size() != crypto_sign_SECRETKEYBYTES) return std::nullopt;

    auto sealed = Identity::from_base64(env.wrapped_key);
    if (sealed.size() < crypto_box_SEALBYTES + kGroupKeyBytes) return std::nullopt;

    // Our Ed25519 secret key -> X25519 keypair.
    uint8_t x_sk[crypto_scalarmult_curve25519_BYTES];
    uint8_t x_pk[crypto_scalarmult_curve25519_BYTES];
    if (crypto_sign_ed25519_sk_to_curve25519(x_sk, our_ed25519_privkey.data()) != 0) {
        return std::nullopt;
    }
    crypto_scalarmult_base(x_pk, x_sk);

    std::vector<uint8_t> out(sealed.size() - crypto_box_SEALBYTES);
    int rc = crypto_box_seal_open(out.data(), sealed.data(), sealed.size(), x_pk, x_sk);
    sodium_memzero(x_sk, sizeof x_sk);
    if (rc != 0) return std::nullopt;  // not addressed to us / corrupt

    auto b64 = Identity::to_base64(out);
    sodium_memzero(out.data(), out.size());
    return b64;
}

} // namespace lnsdk
