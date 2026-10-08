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

#include <cstdint>
#include <memory>
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

    /// Publish a local service at our mesh IP: virtual <mesh_ip>:vport is
    /// bridged to `target` ("tcp:127.0.0.1:PORT" or "unix:/path"), making it
    /// reachable by authorized mesh peers. Returns false if inactive / failed.
    bool tcp_ingress(uint16_t vport, const std::string& target);
    void close_egress(const std::string& dst_ip, uint16_t dst_port);

    /// The dataplane's bound local UDP port (0 when inactive).
    [[nodiscard]] uint16_t bound_port() const;

    /// Sync the desired mesh peer set into the dataplane (add/update/remove).
    /// The server peer is preserved.
    StatusResult sync_peers(const std::vector<MeshPeer>& desired);

    /// Remove a single mesh peer (the server peer is never removed).
    StatusResult remove_peer(const std::string& pubkey);

    /// Live mesh status built from the dataplane's peer snapshot.
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
