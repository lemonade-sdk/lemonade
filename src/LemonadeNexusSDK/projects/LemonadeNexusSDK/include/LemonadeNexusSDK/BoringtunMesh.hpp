#pragma once

/// The client's single userspace mesh dataplane.
///
/// A boringtun Noise dataplane (UserspaceDataplane) joined to an in-process
/// smoltcp netstack — one UDP socket, no TUN device, no kernel interface, no
/// elevated privileges. This replaces the old kernel-style tunnel entirely.
///
/// Two jobs:
///   1. Carry the mesh: peers fetched from the server are synced in via
///      sync_peers(); P2P traffic is cryptokey-routed in userspace.
///   2. Carry the private API: the server's private routes (/api/mesh/peers,
///      /api/relay/list) are only reachable over this plane,
///      so tcp_egress() opens a 127.0.0.1 loopback bridged to the server's
///      tunnel IP through the netstack, and the client speaks plain HTTP to it.

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lnsdk {

class BoringtunMesh {
public:
    BoringtunMesh();
    ~BoringtunMesh();

    BoringtunMesh(const BoringtunMesh&)            = delete;
    BoringtunMesh& operator=(const BoringtunMesh&) = delete;

    /// Bring up the dataplane + netstack from the join-assigned config (our
    /// keys, mesh IP, and the server as the initial peer). Idempotent; returns
    /// false on failure.
    bool start(const BoringtunConfig& config);
    void stop();
    [[nodiscard]] bool is_active() const;

    /// Open (and cache) a loopback TCP egress to dst_ip:dst_port over the mesh.
    /// Returns the bound 127.0.0.1 port, or 0 if inactive / failed. The same
    /// destination reuses one listener.
    uint16_t tcp_egress(const std::string& dst_ip, uint16_t dst_port);

    /// Close the loopback egress to dst_ip:dst_port and abort its open
    /// connections, so a removed or re-addressed peer is no longer reachable
    /// through a stale listener. No-op when inactive.
    void close_egress(const std::string& dst_ip, uint16_t dst_port);

    /// Publish a local service at our mesh IP: virtual <mesh_ip>:vport is
    /// bridged to `target` ("tcp:127.0.0.1:PORT" or "unix:/path"), making it
    /// reachable by authorized mesh peers. Returns false if inactive / failed.
    /// Re-registering a virtual port updates the target for new connections.
    /// With `report_source`, every connection the port accepts is reported to
    /// the ingress source handler (set_ingress_source_handler) before its
    /// first byte reaches `target`; a later registration without it stops
    /// the reports for that port.
    bool tcp_ingress(uint16_t vport, const std::string& target, bool report_source = false);

    /// The source of a connection accepted by an ingress registered with
    /// report_source: the virtual port, the loopback port the bridge connected
    /// to the target from (what the target sees as the remote port; 0 for a
    /// "unix:" target), the peer's tunnel address and port, and whether the
    /// connection opened (true) or ended (false). The dataplane accepts a
    /// packet only when its inner source address is inside the sending peer's
    /// allowed IPs, so the address names the peer the netstack received the
    /// connection from, or the server when the server forwarded it.
    using IngressSourceHandler = std::function<void(uint16_t vport, uint16_t bridge_port,
                                                    const std::string& src_ip, uint16_t src_port,
                                                    bool opened)>;

    /// Set the handler for ingress sources (empty clears it). It runs on the
    /// netstack thread and must return promptly; it must not call this
    /// object. When this returns, the previous handler is not running and is
    /// not called again. Thread-safe.
    void set_ingress_source_handler(IngressSourceHandler handler);

    /// The dataplane's bound local UDP port (0 when inactive).
    [[nodiscard]] uint16_t bound_port() const;

    /// The server peer's endpoint ("host:port") the dataplane started with.
    /// Empty when inactive.
    [[nodiscard]] std::string server_endpoint() const;

    /// Learn this device's reflexive address: send a STUN Binding request
    /// from the dataplane's own UDP socket to `stun_endpoint` ("a.b.c.d:port")
    /// and wait for the matching response. Because the request leaves from
    /// the mesh socket, the answer is the NAT mapping that mesh peers can
    /// punch toward. The request is resent up to `attempts` times within
    /// `timeout`. A response counts only when it comes from `stun_endpoint`,
    /// carries this query's transaction id, and decodes (StunWire). The
    /// result ("a.b.c.d:port") is unauthenticated: treat it as a hint.
    /// nullopt when inactive or on timeout. Blocks the caller for at most
    /// `timeout`. Thread-safe; concurrent queries run one at a time.
    [[nodiscard]] std::optional<std::string> query_reflexive(
        const std::string& stun_endpoint, std::chrono::milliseconds timeout,
        uint32_t attempts = 3);

    /// Hole-punch probe: send a fresh handshake initiation for `pubkey` to
    /// `endpoint` ("a.b.c.d:port") without changing the peer's endpoint (see
    /// UserspaceDataplane::probe_endpoint). Only a valid handshake answer
    /// from that address moves the peer there. False when inactive, the peer
    /// is unknown or is the server, or the endpoint does not parse.
    /// Thread-safe.
    bool probe_peer(const std::string& pubkey, const std::string& endpoint);

    /// Sync the desired mesh peer set into the dataplane (add/update/remove).
    /// The server peer is preserved. Each peer's routes come from
    /// peer_allowed_ips; a peer that gets no route is not added. A peer with
    /// a relay path (set_peer_relay) keeps that path. A peer's endpoint is
    /// written only when the endpoint to use changes (a new listed endpoint,
    /// or a relay path set or cleared). Otherwise the dataplane keeps the
    /// address that the peer's last authenticated packet came from, so a
    /// refresh never undoes roaming or a punched path.
    StatusResult sync_peers(const std::vector<MeshPeer>& desired);

    /// Our mesh public key (base64). Empty when inactive.
    [[nodiscard]] std::string public_key() const;

    /// Rendezvous material for a relay session with `peer_pubkey` (base64) at
    /// the relay `relay_id`, in rendezvous window `window`: 16-byte rendezvous
    /// id, 16-byte own proof, 32-byte commitment to the peer's proof (see
    /// LemonadeNexus/Relay/RelayWire.hpp). It is derived from the X25519
    /// secret this device shares with the peer, so only the two of them can
    /// produce it; the shared secret itself never leaves this object.
    /// nullopt when inactive or a key is invalid. Thread-safe.
    [[nodiscard]] std::optional<std::array<uint8_t, 64>> relay_rendezvous(
        const std::string& peer_pubkey, const std::string& relay_id, uint64_t window) const;

    /// Send `pubkey`'s traffic through a relay: the dataplane sends to
    /// `local_endpoint` (a RelayChannel's 127.0.0.1 port) and starts a new
    /// handshake there. `relay_endpoint` ("ip:port") is reported in
    /// mesh_status(). False when inactive or the peer is unknown. Thread-safe.
    bool set_peer_relay(const std::string& pubkey, const std::string& local_endpoint,
                        const std::string& relay_endpoint);

    /// Stop sending `pubkey`'s traffic through a relay. With an empty
    /// `listed_endpoint`, the next sync_peers writes the endpoint the server
    /// lists. A non-empty `listed_endpoint` means the peer already moved to a
    /// direct path with an authenticated packet: sync_peers keeps that live
    /// address while the server still lists `listed_endpoint`. Thread-safe.
    void clear_peer_relay(const std::string& pubkey, const std::string& listed_endpoint = {});

    /// The allowed-IPs text to install for each peer in `peers`, in the same
    /// order. A peer's addresses become routes on this device, so a peer must
    /// not claim an address that belongs to the server, to this device, or to
    /// another peer:
    ///   - tunnel_ip must be one address ("a.b.c.d" or "a.b.c.d/32") inside
    ///     `tunnel_plane` (the server peer's allowed IPs; 10.64.0.0/10 when
    ///     empty) and must not be `own_tunnel_ip` or `server_tunnel_ip` (the
    ///     session server's address, where the private API answers; empty
    ///     when unknown). A peer route on the server's address would take the
    ///     private API traffic away from the server. A tunnel address that two
    ///     peers claim is refused for both.
    ///   - private_subnet must be a /24 or smaller block inside the private
    ///     pool 10.128.0.0/9 (so it never overlaps the tunnel plane), must
    ///     not contain `own_tunnel_ip` or `server_tunnel_ip`, and must not
    ///     overlap another peer's tunnel address or private subnet.
    ///     Overlapping subnets are refused for every peer involved.
    /// A refused tunnel address gives the peer an empty entry (no route). A
    /// refused subnet leaves the peer its tunnel address only. Pure function.
    [[nodiscard]] static std::vector<std::string> peer_allowed_ips(
        const std::vector<MeshPeer>& peers, const std::vector<std::string>& tunnel_plane,
        const std::string& own_tunnel_ip, const std::string& server_tunnel_ip = {});

    /// Remove a single mesh peer (the server peer is never removed).
    StatusResult remove_peer(const std::string& pubkey);

    /// Live mesh status built from the dataplane's peer snapshot. A relayed
    /// peer has an empty `endpoint` and its relay in `relay_endpoint`. A peer
    /// with a relay path whose traffic now arrives from another address (an
    /// authenticated move, for example to a punched direct path) reports
    /// that address in `endpoint` and an empty `relay_endpoint`.
    [[nodiscard]] MeshTunnelStatus mesh_status() const;

    /// Generate a fresh Curve25519 keypair (base64 {private, public}).
    static std::pair<std::string, std::string> generate_keypair();

    /// Derive a stable Curve25519 mesh keypair from a device seed (base64
    /// {private, public}), domain-separated from other uses of that seed. The
    /// same seed always yields the same pair, so re-joins present the same key.
    static std::pair<std::string, std::string> derive_keypair(std::span<const uint8_t> seed);

    /// The identity-bound mesh keypair: the birational X25519 form of the
    /// device's Ed25519 identity (64-byte libsodium secret key). The server
    /// can derive the public half from the authenticated identity alone, so
    /// possession is proved transitively by the join challenge — no extra
    /// handshake exists or is needed. This supersedes derive_keypair, which
    /// remains only for pre-cutover devices.
    static std::pair<std::string, std::string> identity_bound_keypair(
        std::span<const uint8_t> ed25519_secret_key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
