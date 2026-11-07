#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

#include <sodium.h>

namespace nexus::security {

CanonicalEncoder::CanonicalEncoder(std::string_view domain) {
    add_string(domain);
}

void CanonicalEncoder::put_length_prefixed(std::span<const uint8_t> field) {
    const auto n = static_cast<uint32_t>(field.size());
    for (int i = 0; i < 4; ++i) {
        buffer_.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xFF));
    }
    buffer_.insert(buffer_.end(), field.begin(), field.end());
}

void CanonicalEncoder::add_bytes(std::span<const uint8_t> field) {
    put_length_prefixed(field);
}

void CanonicalEncoder::add_string(std::string_view field) {
    put_length_prefixed({reinterpret_cast<const uint8_t*>(field.data()), field.size()});
}

void CanonicalEncoder::add_u16(uint16_t value) {
    std::array<uint8_t, 2> bytes{};
    for (int i = 0; i < 2; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    put_length_prefixed(bytes);
}

void CanonicalEncoder::add_u32(uint32_t value) {
    std::array<uint8_t, 4> bytes{};
    for (int i = 0; i < 4; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    put_length_prefixed(bytes);
}

void CanonicalEncoder::add_u64(uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    put_length_prefixed(bytes);
}

Digest CanonicalEncoder::digest() const {
    Digest out{};
    crypto_hash_sha256(out.data(), buffer_.data(), buffer_.size());
    return out;
}

}  // namespace nexus::security
