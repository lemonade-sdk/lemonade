#pragma once

/// LAN-direct paths for the BoringTun mesh dataplane.
///
/// Each device advertises its LAN addresses, paired with the dataplane's UDP
/// port, in the mesh heartbeat (local_endpoints). The server lists them only
/// to the peers that already list the device. A device probes the LAN
/// endpoints of a peer on its own network before or with the peer's public
/// endpoint, so two devices behind a router that does not hairpin UDP still
/// meet directly. A LAN endpoint only changes where packets go: the
/// BoringTun handshake still authenticates the peer by its mesh key.
///
/// The functions here choose which local addresses to advertise and which
/// peer LAN endpoints to probe. list_interface_addresses() reads the host's
/// interfaces (getifaddrs on POSIX, iOS and Android; GetAdaptersAddresses on
/// Windows); the other functions are pure.

#include <LemonadeNexusSDK/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lnsdk {

/// Most LAN endpoints a device advertises. The server refuses a heartbeat
/// with more (kMaxMeshLocalEndpoints).
inline constexpr std::size_t kMaxLocalEndpoints = 4;

/// One IPv4 address of a local network interface.
struct InterfaceAddress {
    std::string name;             ///< Interface name ("en0", "wlan0", "Wi-Fi")
    std::string address;          ///< Dotted IPv4 address
    uint8_t     prefix_len{32};   ///< Subnet prefix length, 0 to 32
    bool        up{true};         ///< The interface is up and running
    bool        loopback{false};  ///< A loopback interface
    /// A point-to-point link or an adapter type that the platform reports as
    /// a tunnel or VPN (for example PPP or a tunnel adapter on Windows).
    bool        tunnel{false};
};

/// The IPv4 addresses of this host's interfaces, in the order the platform
/// lists them. Empty when the platform call fails. Thread-safe.
[[nodiscard]] std::vector<InterfaceAddress> list_interface_addresses();

/// Whether an interface name names a VPN, tunnel, container, or other
/// virtual interface whose address is not a LAN address that peers can
/// reach: utun, tun, tap, wg, ppp, ipsec, gif, stf, awdl, llw, docker, veth,
/// br-, virbr, vmnet, vboxnet, zt, tailscale, and the cellular interfaces
/// rmnet, ccmni, pdp_ip, clat. Case-insensitive prefix match. Pure.
[[nodiscard]] bool virtual_interface_name(std::string_view name);

/// What select_local_endpoints needs besides the interface list.
struct LocalEndpointRules {
    /// CIDRs of the mesh's own address plane ("10.64.0.0/10"); addresses
    /// inside them are never advertised.
    std::vector<std::string> mesh_ranges;
    /// This device's reflexive endpoint ("a.b.c.d:port"; empty when unknown).
    /// A shared address space address (100.64.0.0/10) is advertised only
    /// when it equals the reflexive address: then no NAT is between this
    /// device and the STUN server, and the address is the device's own.
    std::string reflexive;
    /// Advertise loopback addresses. Only for a single-host deployment: the
    /// session server is on loopback, as peer_endpoint_usable requires.
    bool allow_loopback{false};
};

/// The usable LAN addresses among `interfaces`, in list order, without
/// duplicates and at most kMaxLocalEndpoints. An address is usable when its
/// interface is up, is not a tunnel (InterfaceAddress::tunnel), and has no
/// virtual_interface_name, and the address is in 10.0.0.0/8, 172.16.0.0/12,
/// or 192.168.0.0/16, or is the reflexive address in 100.64.0.0/10, and is
/// outside rules.mesh_ranges. Loopback interfaces and addresses count only
/// with rules.allow_loopback. Link-local, public, and invalid addresses never
/// count. Pure.
[[nodiscard]] std::vector<InterfaceAddress> usable_lan_interfaces(
    const std::vector<InterfaceAddress>& interfaces, const LocalEndpointRules& rules);

/// The LAN endpoints to advertise: "address:port" for each address of
/// usable_lan_interfaces. Empty when `port` is 0. Pure.
[[nodiscard]] std::vector<std::string> select_local_endpoints(
    const std::vector<InterfaceAddress>& interfaces, uint16_t port,
    const LocalEndpointRules& rules);

/// The LAN endpoints of `peer` that this device probes, or none when the
/// peer is not on this device's network. `peer.local_endpoints` must already
/// hold only endpoints that passed lan_endpoint_usable. The peer is on this
/// device's network when its listed public endpoint has the same IP address
/// as `own_reflexive` (one NAT for both), or when one of its LAN endpoints
/// is inside the subnet of one of `own_lan` (usable_lan_interfaces). Then
/// all its LAN endpoints are returned, in list order. Pure.
[[nodiscard]] std::vector<std::string> lan_probe_targets(const MeshPeer& peer,
                                                         const std::string& own_reflexive,
                                                         const std::vector<InterfaceAddress>& own_lan);

/// Whether a LAN endpoint that the server lists for a peer may be probed: a
/// dotted "a.b.c.d:port" whose address is private (10/8, 172.16/12,
/// 192.168/16, 100.64/10), or loopback when `server_on_loopback`. Pure.
[[nodiscard]] bool lan_endpoint_usable(const std::string& endpoint, bool server_on_loopback);

}  // namespace lnsdk
