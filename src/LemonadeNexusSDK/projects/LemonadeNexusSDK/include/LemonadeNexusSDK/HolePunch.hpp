#pragma once

/// Direct peer-to-peer paths (UDP hole punching) for the BoringTun mesh
/// dataplane.
///
/// Each device learns its reflexive address with a STUN query sent from the
/// dataplane socket (BoringtunMesh::query_reflexive) and publishes it with the
/// mesh heartbeat. The server stores it in the tree, and every authorized peer
/// receives it from the verified Tier 1 server in GET /api/mesh/peers.
///
/// HolePuncher then sends probes: BoringTun handshake initiations from the
/// dataplane socket to the peer's listed endpoint (BoringtunMesh::probe_peer).
/// For a peer on this device's network, each probe goes to the peer's LAN
/// endpoints first (MeshPeer::local_endpoints, see LanPaths.hpp) and to its
/// listed endpoint in the same moment: one initiation, sent to every
/// address, so a router that does not hairpin UDP still lets the two
/// devices meet on the LAN.
/// A probe opens this side's NAT mapping toward the peer. When both sides
/// probe, the second probe to arrive passes the other NAT and the handshake
/// completes. A probe changes no state: only a valid handshake answer moves
/// the peer to the punched address (authenticated roaming in the dataplane).
///
/// Schedule per peer (a probe target is a LAN endpoint or the listed
/// endpoint):
///   - Direct: the peer has a probe target, no relay path, and no fresh
///     handshake. A burst of probes, then a few probes at each pair slot.
///   - Upgrade: the peer goes through a relay and prefer_direct is set. A few
///     probes at each pair slot. A punched path replaces the relay
///     (RelayFallback closes the relay channel). A peer that moves onto a
///     relay during its Direct burst finishes that burst first.
///   - Idle: a fresh handshake on a direct path, no probe target, or a
///     relayed peer without prefer_direct.
/// renew() and burst_now() start a new burst at the next pass, outside the
/// slot schedule: the mesh orchestrator calls them when a peer's endpoint
/// changed, when the peer list was refreshed on request, and after the
/// process resumed from a suspension.
/// A pair slot is a wall-clock time that both peers compute from their two
/// public keys (pair_slot_offset), so both sides probe at about the same
/// moment without extra signaling. This works when the clocks of the two
/// devices agree to within the NAT mapping lifetime (normally 30 s or more).
///
/// Not thread-safe except probes_sent(): call maintain() from one thread (the
/// mesh orchestrator's).

#include <LemonadeNexusSDK/Types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lnsdk {

class BoringtunMesh;

/// Reachability scope of an IPv4 address.
enum class AddressScope : uint8_t {
    Invalid,   ///< Does not parse, or is not a usable unicast address.
    Loopback,  ///< 127.0.0.0/8
    Private,   ///< 10/8, 172.16/12, 192.168/16, and 100.64/10 (shared address space)
    Public,    ///< Every other unicast address.
};

/// Scope of an "a.b.c.d:port" endpoint. Invalid when the text is not a dotted
/// IPv4 address with a port from 1 to 65535, or when the address is
/// unspecified (0.0.0.0/8), link-local (169.254/16), multicast (224/4), or
/// reserved or broadcast (240/4).
[[nodiscard]] AddressScope endpoint_scope(const std::string& endpoint);

/// Whether a STUN-reported reflexive address may be published as this
/// device's mesh endpoint. A STUN server can only observe an address in its
/// own network scope, so a public reflexive address is always accepted, a
/// private one only from a private or loopback STUN server, and a loopback
/// one only from a loopback STUN server. `stun_server_ip` is the dotted IPv4
/// address the query went to.
[[nodiscard]] bool reflexive_publishable(const std::string& reflexive,
                                         const std::string& stun_server_ip);

/// Whether a peer endpoint that the server lists may be probed. Public and
/// private addresses are accepted (a private address is a peer on the same
/// network as its STUN server). A loopback address is accepted only when the
/// session server itself is on loopback (a single-host deployment), so a
/// listed endpoint cannot aim probes at local services. `server_scope` is
/// the scope of the session server's mesh endpoint.
[[nodiscard]] bool peer_endpoint_usable(const std::string& endpoint, AddressScope server_scope);

class HolePuncher {
public:
    enum class Mode : uint8_t { Idle, Direct, Upgrade };

    struct Timing {
        /// Gap between the probes of one burst.
        std::chrono::milliseconds burst_interval{1000};
        /// Probes in the first burst toward a new endpoint (Direct mode).
        uint32_t burst_count{5};
        /// Probes sent at each pair slot.
        uint32_t slot_burst{2};
        /// Slot period in Direct mode, after the first burst.
        std::chrono::milliseconds slot_interval{15000};
        /// Slot period in Upgrade mode.
        std::chrono::milliseconds upgrade_interval{60000};
        /// A handshake younger than this, in seconds, is a working path.
        int64_t handshake_fresh_sec{180};
    };

    /// Most peers probed in one pass. A probe to one peer goes to at most
    /// kMaxLocalEndpoints + 1 addresses. Together with the minimum pass gap
    /// this limits the probe rate.
    static constexpr std::size_t kMaxProbesPerPass = 16;
    /// The earliest next pass that maintain() reports.
    static constexpr std::chrono::milliseconds kMinPassGap{250};
    /// The latest next pass that maintain() reports.
    static constexpr std::chrono::milliseconds kMaxPassGap{5000};

    explicit HolePuncher(BoringtunMesh& mesh);
    HolePuncher(BoringtunMesh& mesh, Timing timing);

    HolePuncher(const HolePuncher&) = delete;
    HolePuncher& operator=(const HolePuncher&) = delete;

    /// One pass over the authorized peer list. `peers` must hold only
    /// endpoints that passed peer_endpoint_usable, and only LAN endpoints
    /// that lan_probe_targets chose. `relayed` maps the mesh
    /// public keys of relayed peers to their relay (RelayFallback::
    /// relayed_peers). Returns the steady-clock time of the next due probe,
    /// from kMinPassGap to kMaxPassGap ahead.
    std::chrono::steady_clock::time_point maintain(
        const std::vector<MeshPeer>& peers, const std::map<std::string, std::string>& relayed,
        bool prefer_direct);

    /// Forget every peer schedule.
    void reset();

    /// The paths to these peers (mesh public keys) may have changed: the
    /// server lists a new endpoint, the peer came back online, or this
    /// device resumed from a suspension. A handshake from before this call
    /// no longer counts as a fresh path for them, and each gets a new burst
    /// of probes at the next pass, also when it is relayed and prefer_direct
    /// is set. A handshake after this call ends the renewal. Peers that the
    /// next pass does not list are ignored.
    void renew(const std::vector<std::string>& pubkeys);

    /// Start a new burst at the next pass for every peer that is being
    /// probed (Direct or Upgrade), instead of waiting for its next slot.
    void burst_now();

    /// Probe datagrams sent since construction; a probe to several
    /// addresses counts once per address. Thread-safe.
    [[nodiscard]] uint64_t probes_sent() const { return probes_sent_.load(); }

    /// The probe targets of a peer: its LAN endpoints, then its listed
    /// endpoint, without empty or repeated entries. Pure.
    [[nodiscard]] static std::vector<std::string> probe_targets(const MeshPeer& peer);

    /// The schedule a peer needs now. Pure.
    /// @param has_endpoint    the peer has a probe target (probe_targets)
    /// @param relayed         the peer's traffic goes through a relay
    /// @param last_handshake  Unix seconds of the last handshake, 0 if none
    /// @param now_unix        Unix seconds now
    [[nodiscard]] static Mode punch_mode(bool has_endpoint, bool relayed, int64_t last_handshake,
                                         int64_t now_unix, bool prefer_direct,
                                         const Timing& timing);

    /// The slot offset of a pair of public keys, in milliseconds below
    /// `interval_ms`. Both peers compute the same value in either key order
    /// (FNV-1a over the sorted keys; a schedule, not a security value). Pure.
    [[nodiscard]] static int64_t pair_slot_offset(const std::string& key_a,
                                                  const std::string& key_b, int64_t interval_ms);

    /// Milliseconds from `now_unix_ms` to the next slot: the first later time
    /// t with (t + offset_ms) a multiple of `interval_ms`. From 1 to
    /// interval_ms. Pure.
    [[nodiscard]] static int64_t next_slot_delay(int64_t now_unix_ms, int64_t interval_ms,
                                                 int64_t offset_ms);

private:
    using Clock = std::chrono::steady_clock;

    struct PeerState {
        std::vector<std::string> targets;  ///< probe_targets()
        Mode              mode{Mode::Idle};
        uint32_t          burst_left{0};
        Clock::time_point next{};
    };

    Clock::time_point next_slot(Clock::time_point now, const std::string& own,
                                const std::string& peer, Mode mode) const;

    BoringtunMesh&                   mesh_;
    Timing                           timing_;
    std::map<std::string, PeerState> peers_;  // by mesh public key
    /// Renewed peers: mesh public key -> Unix seconds of the renewal. A
    /// handshake at or before that time does not count as fresh.
    std::map<std::string, int64_t>   stale_before_;
    std::set<std::string>            burst_next_;  // renewed peers due a new burst
    std::atomic<uint64_t>            probes_sent_{0};
};

}  // namespace lnsdk
