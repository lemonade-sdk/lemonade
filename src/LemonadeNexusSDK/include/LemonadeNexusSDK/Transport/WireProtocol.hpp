#pragma once

/// Minimal WireGuard wire-format and IPv4 header parsing.
///
/// The WireGuard message layout is a stable, public protocol
/// (https://www.wireguard.com/protocol/). We parse just enough here to
/// demultiplex incoming UDP datagrams to the right per-peer Noise session
/// without touching any cryptography:
///
///   type 1  handshake initiation (148 B)  sender_index    @ [4..8)
///   type 2  handshake response   (92 B)   receiver_index  @ [8..12)
///   type 3  cookie reply         (64 B)   receiver_index  @ [4..8)
///   type 4  transport data       (>=32 B) receiver_index  @ [4..8)
///
/// BoringTun assigns local session receiver indices as
/// (tunnel_index << 8) | counter, so `receiver_index >> 8` recovers the
/// tunnel index we passed to new_tunnel() — an O(1) peer lookup key.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace nexus::boringtun::wire {

enum class MsgType : uint32_t {
    HandshakeInit     = 1,
    HandshakeResponse = 2,
    CookieReply       = 3,
    TransportData     = 4,
};

inline constexpr size_t kHandshakeInitSize     = 148;
inline constexpr size_t kHandshakeResponseSize = 92;
inline constexpr size_t kCookieReplySize       = 64;
inline constexpr size_t kTransportDataMinSize  = 32;

/// Worst-case overhead added by encapsulation (handshake initiation size).
inline constexpr size_t kMaxOverhead = 148;

namespace detail {
inline uint32_t read_le32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}
} // namespace detail

/// Parse the message type. The full first 4 bytes are the little-endian type
/// (one type byte + three reserved zero bytes); anything else is not a valid
/// WireGuard packet. Also enforces the per-type size constraints.
inline std::optional<MsgType> parse_type(std::span<const uint8_t> pkt) {
    if (pkt.size() < 4) return std::nullopt;
    switch (detail::read_le32(pkt.data())) {
        case 1: return pkt.size() == kHandshakeInitSize
                       ? std::optional{MsgType::HandshakeInit} : std::nullopt;
        case 2: return pkt.size() == kHandshakeResponseSize
                       ? std::optional{MsgType::HandshakeResponse} : std::nullopt;
        case 3: return pkt.size() == kCookieReplySize
                       ? std::optional{MsgType::CookieReply} : std::nullopt;
        case 4: return pkt.size() >= kTransportDataMinSize
                       ? std::optional{MsgType::TransportData} : std::nullopt;
        default: return std::nullopt;
    }
}

/// Extract our session receiver index from a response/cookie/transport packet.
/// Returns nullopt for handshake initiations (they carry only the *sender's*
/// index) and malformed packets.
inline std::optional<uint32_t> receiver_index(std::span<const uint8_t> pkt) {
    auto type = parse_type(pkt);
    if (!type) return std::nullopt;
    switch (*type) {
        case MsgType::HandshakeResponse: return detail::read_le32(pkt.data() + 8);
        case MsgType::CookieReply:
        case MsgType::TransportData:     return detail::read_le32(pkt.data() + 4);
        case MsgType::HandshakeInit:     return std::nullopt;
    }
    return std::nullopt;
}

/// Recover the per-peer tunnel index from a session receiver index.
inline constexpr uint32_t peer_index(uint32_t receiver_idx) {
    return receiver_idx >> 8;
}

// ---------------------------------------------------------------------------
// IPv4 header helpers (for userspace routing of decrypted packets)
// ---------------------------------------------------------------------------

namespace ipv4 {

inline constexpr size_t kMinHeaderSize = 20;

/// True if the buffer starts with an IPv4 header (version nibble == 4).
inline bool is_ipv4(std::span<const uint8_t> pkt) {
    return pkt.size() >= kMinHeaderSize && (pkt[0] >> 4) == 4;
}

/// Source address in host byte order, or nullopt if not IPv4.
inline std::optional<uint32_t> src_addr(std::span<const uint8_t> pkt) {
    if (!is_ipv4(pkt)) return std::nullopt;
    return (uint32_t(pkt[12]) << 24) | (uint32_t(pkt[13]) << 16) |
           (uint32_t(pkt[14]) << 8)  |  uint32_t(pkt[15]);
}

/// Destination address in host byte order, or nullopt if not IPv4.
inline std::optional<uint32_t> dst_addr(std::span<const uint8_t> pkt) {
    if (!is_ipv4(pkt)) return std::nullopt;
    return (uint32_t(pkt[16]) << 24) | (uint32_t(pkt[17]) << 16) |
           (uint32_t(pkt[18]) << 8)  |  uint32_t(pkt[19]);
}

} // namespace ipv4

} // namespace nexus::boringtun::wire
