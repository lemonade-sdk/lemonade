#pragma once

/// Tunnel MTU discovery for the client dataplane. The client sends echo
/// requests of several sizes to the session server through the tunnel and
/// makes the largest size that is answered its netstack MTU
/// (BoringtunMesh::probe_tunnel_mtu). TCP on both ends then sizes every
/// segment to it, since the netstack advertises its MTU as the segment size
/// it accepts.

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace lnsdk::tunnel_mtu {

/// The WireGuard convention for a 1500-byte underlay; every stack starts here.
inline constexpr uint32_t kFull = 1420;

/// Used when no probe is answered: the IPv6 minimum link MTU, which every
/// tunnel and carrier path must carry.
inline constexpr uint32_t kFailsafe = 1280;

/// MTU candidates, largest first.
inline constexpr std::array<uint32_t, 4> kLadder{1420, 1340, 1280, 1200};

/// The echo request length that stands for `mtu`: kFull itself, and below
/// it `mtu` plus the relay packet header, so the size chosen also fits the
/// path to a relayed peer.
uint32_t probe_size(uint32_t mtu);

/// An IPv4 ICMP echo request of `total_len` bytes (at least 28) with valid
/// header and ICMP checksums. Addresses are in host byte order.
std::vector<uint8_t> echo_request(uint32_t src, uint32_t dst, uint16_t ident, uint16_t seq,
                                  uint32_t total_len);

/// The length of `pkt` when it is an IPv4 ICMP echo reply carrying `ident`,
/// else nullopt.
std::optional<uint32_t> echo_reply_len(std::span<const uint8_t> pkt, uint16_t ident);

/// The MTU to use when echo replies of the `answered` lengths came back: the
/// largest ladder size whose probe_size is among them, kFailsafe when none is.
uint32_t choose(const std::set<uint32_t>& answered);

}  // namespace lnsdk::tunnel_mtu
