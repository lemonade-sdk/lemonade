#pragma once

/// STUN (RFC 5389) Binding wire codec shared by the server's StunService and
/// the SDK's reflexive-address query, so both sides encode the same bytes.
///
/// Only the subset the mesh uses is supported: a Binding request with no
/// attributes, and a Binding success response that carries an IPv4
/// XOR-MAPPED-ADDRESS. A STUN response is not authenticated. A caller must
/// treat the address it carries as a hint, never as proof of reachability.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::network::stun {

inline constexpr uint16_t    kBindingRequest       = 0x0001;
inline constexpr uint16_t    kBindingResponse      = 0x0101;
inline constexpr uint32_t    kMagicCookie          = 0x2112A442;
inline constexpr uint16_t    kAttrXorMappedAddress = 0x0020;
inline constexpr std::size_t kHeaderSize           = 20;  ///< type, length, cookie, transaction id
/// Largest message accepted. A Binding response with one address is 32 bytes;
/// RFC 5389 keeps messages without fragmentation under 548 bytes.
inline constexpr std::size_t kMaxMessageSize       = 548;

/// The 96-bit transaction id that pairs a response with its request.
using TransactionId = std::array<uint8_t, 12>;

/// An IPv4 transport address in host byte order.
struct MappedAddress {
    uint32_t ipv4{0};
    uint16_t port{0};
};

/// True when `datagram` has a well-formed STUN header: the two top bits of
/// the type are zero, the magic cookie is present, and the length field
/// equals the attribute bytes that follow (a multiple of 4). It never matches
/// a BoringTun datagram, whose first four bytes are a small type value.
[[nodiscard]] bool is_stun_message(std::span<const uint8_t> datagram);

/// The message type of a well-formed STUN message, nullopt otherwise.
[[nodiscard]] std::optional<uint16_t> message_type(std::span<const uint8_t> datagram);

/// A Binding request with no attributes (20 bytes).
[[nodiscard]] std::vector<uint8_t> encode_binding_request(const TransactionId& txn);

/// A Binding success response for `txn` that carries `mapped` as an IPv4
/// XOR-MAPPED-ADDRESS (32 bytes).
[[nodiscard]] std::vector<uint8_t> encode_binding_response(const TransactionId& txn,
                                                           const MappedAddress& mapped);

/// Decode a Binding success response. Returns nullopt when the message is
/// malformed, is not a Binding success response, names a transaction id other
/// than `expected`, has an attribute that overruns the message, or has no IPv4
/// XOR-MAPPED-ADDRESS with a nonzero port and address.
[[nodiscard]] std::optional<MappedAddress> decode_binding_response(
    std::span<const uint8_t> datagram, const TransactionId& expected);

}  // namespace nexus::network::stun
