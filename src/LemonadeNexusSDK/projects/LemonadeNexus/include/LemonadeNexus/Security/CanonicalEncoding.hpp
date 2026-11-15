#pragma once

// The single canonical serialization for security digests and signatures.
//
// The architecture leaves the exact serialization to the implementation; this
// encoder is that definition. Every signed or digested security object goes
// through it — a second encoding of "the same" object is how a verifier and a
// signer end up agreeing on different bytes.
//
// Layout: each field is a little-endian u32 byte length followed by the field
// bytes. The domain string is the first field. Integers encode fixed-width
// little-endian and are length-prefixed like every other field, so no two
// distinct field sequences can produce the same byte stream.

#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace nexus::security {

class CanonicalEncoder {
public:
    explicit CanonicalEncoder(std::string_view domain);

    void add_bytes(std::span<const uint8_t> field);
    void add_string(std::string_view field);
    void add_u16(uint16_t value);
    void add_u32(uint32_t value);
    void add_u64(uint64_t value);

    [[nodiscard]] const std::vector<uint8_t>& bytes() const { return buffer_; }

    /// SHA-256 over the encoded stream.
    [[nodiscard]] Digest digest() const;

private:
    void put_length_prefixed(std::span<const uint8_t> field);

    std::vector<uint8_t> buffer_;
};

}  // namespace nexus::security
