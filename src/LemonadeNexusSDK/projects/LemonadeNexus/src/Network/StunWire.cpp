#include <LemonadeNexus/Network/StunWire.hpp>

#include <algorithm>

namespace nexus::network::stun {

namespace {

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void write_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

std::vector<uint8_t> header(uint16_t type, uint16_t length, const TransactionId& txn) {
    std::vector<uint8_t> out(kHeaderSize + length, 0);
    write_u16(out.data(), type);
    write_u16(out.data() + 2, length);
    write_u32(out.data() + 4, kMagicCookie);
    std::copy(txn.begin(), txn.end(), out.begin() + 8);
    return out;
}

}  // namespace

bool is_stun_message(std::span<const uint8_t> datagram) {
    if (datagram.size() < kHeaderSize || datagram.size() > kMaxMessageSize) return false;
    if ((datagram[0] & 0xC0) != 0) return false;
    if (read_u32(datagram.data() + 4) != kMagicCookie) return false;
    const std::size_t length = read_u16(datagram.data() + 2);
    return length % 4 == 0 && length == datagram.size() - kHeaderSize;
}

std::optional<uint16_t> message_type(std::span<const uint8_t> datagram) {
    if (!is_stun_message(datagram)) return std::nullopt;
    return read_u16(datagram.data());
}

std::vector<uint8_t> encode_binding_request(const TransactionId& txn) {
    return header(kBindingRequest, 0, txn);
}

std::vector<uint8_t> encode_binding_response(const TransactionId& txn,
                                             const MappedAddress& mapped) {
    // XOR-MAPPED-ADDRESS: 4-byte attribute header, then reserved, family,
    // X-Port (port XOR the cookie's high 16 bits), X-Address (address XOR
    // the cookie).
    constexpr uint16_t kValueLen = 8;
    constexpr uint16_t kAttrLen = 4 + kValueLen;
    auto out = header(kBindingResponse, kAttrLen, txn);
    uint8_t* attr = out.data() + kHeaderSize;
    write_u16(attr, kAttrXorMappedAddress);
    write_u16(attr + 2, kValueLen);
    attr[4] = 0x00;
    attr[5] = 0x01;  // IPv4
    write_u16(attr + 6, static_cast<uint16_t>(mapped.port ^ (kMagicCookie >> 16)));
    write_u32(attr + 8, mapped.ipv4 ^ kMagicCookie);
    return out;
}

std::optional<MappedAddress> decode_binding_response(std::span<const uint8_t> datagram,
                                                     const TransactionId& expected) {
    if (message_type(datagram) != kBindingResponse) return std::nullopt;
    if (!std::equal(expected.begin(), expected.end(), datagram.begin() + 8)) return std::nullopt;

    std::size_t offset = kHeaderSize;
    while (offset + 4 <= datagram.size()) {
        const uint16_t type = read_u16(datagram.data() + offset);
        const std::size_t len = read_u16(datagram.data() + offset + 2);
        const std::size_t padded = (len + 3) & ~std::size_t{3};
        if (offset + 4 + padded > datagram.size()) return std::nullopt;
        if (type == kAttrXorMappedAddress) {
            const uint8_t* value = datagram.data() + offset + 4;
            if (len != 8 || value[1] != 0x01) return std::nullopt;  // IPv4 only
            MappedAddress mapped;
            mapped.port = static_cast<uint16_t>(read_u16(value + 2) ^ (kMagicCookie >> 16));
            mapped.ipv4 = read_u32(value + 4) ^ kMagicCookie;
            if (mapped.port == 0 || mapped.ipv4 == 0) return std::nullopt;
            return mapped;
        }
        offset += 4 + padded;
    }
    return std::nullopt;
}

}  // namespace nexus::network::stun
