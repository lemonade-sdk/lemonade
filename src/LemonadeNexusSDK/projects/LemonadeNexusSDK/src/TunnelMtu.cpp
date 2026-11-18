#include <LemonadeNexusSDK/TunnelMtu.hpp>

#include <algorithm>

namespace lnsdk::tunnel_mtu {

namespace {

uint16_t internet_checksum(std::span<const uint8_t> bytes) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        sum += (uint32_t{bytes[i]} << 8) | bytes[i + 1];
    }
    if (bytes.size() % 2 == 1) sum += uint32_t{bytes.back()} << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

void put16(std::vector<uint8_t>& pkt, size_t at, uint16_t value) {
    pkt[at]     = static_cast<uint8_t>(value >> 8);
    pkt[at + 1] = static_cast<uint8_t>(value & 0xFF);
}

void put32(std::vector<uint8_t>& pkt, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        pkt[at + i] = static_cast<uint8_t>(value >> (8 * (3 - i)));
    }
}

}  // namespace

std::vector<uint8_t> echo_request(uint32_t src, uint32_t dst, uint16_t ident, uint16_t seq,
                                  uint32_t total_len) {
    std::vector<uint8_t> pkt(std::max<uint32_t>(total_len, 28), 0);
    pkt[0] = 0x45;
    put16(pkt, 2, static_cast<uint16_t>(pkt.size()));
    pkt[8] = 64;
    pkt[9] = 1;
    put32(pkt, 12, src);
    put32(pkt, 16, dst);
    put16(pkt, 10, internet_checksum({pkt.data(), 20}));
    pkt[20] = 8;
    put16(pkt, 24, ident);
    put16(pkt, 26, seq);
    put16(pkt, 22, internet_checksum({pkt.data() + 20, pkt.size() - 20}));
    return pkt;
}

std::optional<uint32_t> echo_reply_len(std::span<const uint8_t> pkt, uint16_t ident) {
    if (pkt.size() < 28 || (pkt[0] >> 4) != 4) return std::nullopt;
    const size_t ihl = static_cast<size_t>(pkt[0] & 0x0F) * 4;
    if (ihl < 20 || pkt.size() < ihl + 8) return std::nullopt;
    if (pkt[9] != 1 || pkt[ihl] != 0) return std::nullopt;
    const uint16_t got = static_cast<uint16_t>((pkt[ihl + 4] << 8) | pkt[ihl + 5]);
    if (got != ident) return std::nullopt;
    return static_cast<uint32_t>(pkt.size());
}

uint32_t choose(const std::set<uint32_t>& answered) {
    for (uint32_t size : kLadder) {
        if (answered.count(size)) return size;
    }
    return kFailsafe;
}

}  // namespace lnsdk::tunnel_mtu
