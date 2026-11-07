#include <LemonadeNexus/Crypto/CryptoTypes.hpp>

#include <sodium.h>

#include <stdexcept>

namespace nexus::crypto {

std::string to_base64(std::span<const uint8_t> data) {
    const auto encoded_len = sodium_base64_encoded_len(data.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string encoded(encoded_len, '\0');
    sodium_bin2base64(encoded.data(), encoded_len,
                       data.data(), data.size(),
                       sodium_base64_VARIANT_ORIGINAL);
    // Remove null terminator added by sodium
    if (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
}

std::vector<uint8_t> from_base64(std::string_view encoded) {
    std::vector<uint8_t> decoded(encoded.size()); // over-allocate, will resize
    std::size_t decoded_len = 0;
    // Try standard base64 first, then URL-safe (no padding) for client SDK compat
    if (sodium_base642bin(decoded.data(), decoded.size(),
                           encoded.data(), encoded.size(),
                           nullptr, &decoded_len, nullptr,
                           sodium_base64_VARIANT_ORIGINAL) == 0) {
        decoded.resize(decoded_len);
        return decoded;
    }
    decoded_len = 0;
    if (sodium_base642bin(decoded.data(), decoded.size(),
                           encoded.data(), encoded.size(),
                           nullptr, &decoded_len, nullptr,
                           sodium_base64_VARIANT_URLSAFE_NO_PADDING) == 0) {
        decoded.resize(decoded_len);
        return decoded;
    }
    throw std::runtime_error("Invalid base64 input");
}

std::string canonical_key_b64(std::string_view encoded) {
    constexpr std::string_view prefix = "ed25519:";
    if (encoded.starts_with(prefix)) encoded.remove_prefix(prefix.size());
    if (encoded.empty()) return {};
    try {
        return to_base64(from_base64(encoded));
    } catch (const std::exception&) {
        return {};   // undecodable: callers treat empty as "no identity"
    }
}

std::string to_hex(std::span<const uint8_t> data) {
    std::string hex(data.size() * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), data.data(), data.size());
    hex.pop_back(); // remove null terminator
    return hex;
}

std::vector<uint8_t> from_hex(std::string_view hex) {
    std::vector<uint8_t> bin(hex.size() / 2);
    std::size_t bin_len = 0;
    if (sodium_hex2bin(bin.data(), bin.size(),
                        hex.data(), hex.size(),
                        nullptr, &bin_len, nullptr) != 0) {
        throw std::runtime_error("Invalid hex input");
    }
    bin.resize(bin_len);
    return bin;
}

} // namespace nexus::crypto
