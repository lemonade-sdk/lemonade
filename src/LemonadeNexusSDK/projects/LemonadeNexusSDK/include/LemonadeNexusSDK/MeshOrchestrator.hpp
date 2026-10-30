#pragma once

#include <LemonadeNexusSDK/HolePunch.hpp>
#include <LemonadeNexusSDK/RelayFallback.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lnsdk {

// Forward declarations — avoid including full headers
class LemonadeNexusClient;
class BoringtunMesh;

/// Whether a peer's newest liveness signal is fresh at `now_unix` (Unix
/// seconds), or nullopt when the peer has no signal yet. Pure.
///
/// The rule: an authenticated packet from the peer (last_rx, keepalives
/// included) within three keepalive intervals plus one monitor tick is
/// fresh. Before the first packet, a live session (last_handshake within
/// 200 s: BoringTun's REJECT_AFTER_TIME of 180 s, one retry, and jitter) is
/// fresh. The server's last_seen within 15 s plus one peer refresh interval
/// is fresh before the first packet, and also for a relayed peer
/// (relay_endpoint set): a relay can delay its packets while the server
/// still hears from it.
[[nodiscard]] std::optional<bool> peer_signal_fresh(const MeshPeer& peer, int64_t now_unix,
                                                    uint32_t peer_refresh_interval_sec);

/// Monitor passes in a row that must find an online peer's signal stale
/// before the peer reads offline. One late packet or peer refresh does not
/// flip it.
inline constexpr uint32_t kPeerOfflinePasses = 2;

/// A peer's online state between monitor passes.
struct PeerLiveness {
    bool     online{false};
    uint32_t stale_passes{0};  ///< Stale passes in a row while still online
};

/// The online state after one monitor pass that found the peer's signal
/// `fresh` or stale. A fresh signal makes the peer online at once. A stale
/// signal makes it offline on the kPeerOfflinePasses-th stale pass in a row.
/// Pure.
[[nodiscard]] PeerLiveness next_peer_liveness(PeerLiveness previous, bool fresh);

/// Background orchestrator for P2P mesh networking over the boringtun dataplane.
///
/// Periodically:
///   1. Fetches authorized peers from the server (GET /api/mesh/peers/{node_id})
///   2. Syncs the mesh dataplane's peer set (add/remove/update)
///   3. Learns our reflexive address with a STUN query from the dataplane
///      socket to the session server (port 3478), every
///      stun_refresh_interval_sec (0 turns it off)
///   4. Reports that endpoint via heartbeat (POST /api/mesh/heartbeat). An
///      empty endpoint (none learned yet) leaves the stored endpoint as is.
///   5. Punches direct paths to peers at their listed endpoints (HolePuncher)
///   6. Monitors peer connectivity and fires state callbacks
///   7. Moves peers without a direct path onto a relay (RelayFallback): the
///      relay ticket comes from the session's verified Tier 1 server, and the
///      relays are the verified Tier 1 servers plus the relays the server lists.
///      A relayed peer moves back to a direct path when a punch succeeds and
///      prefer_direct is set.
///   8. Checks the client's registered certificates and renews due ones on a
///      separate thread, so a slow certificate request never delays steps 1-7
///
/// Peer endpoints come only from the verified server's peer list. Listed
/// endpoints that fail peer_endpoint_usable are dropped before any use.
class MeshOrchestrator {
public:
    using StateCallback = std::function<void(const MeshTunnelStatus&)>;

    MeshOrchestrator(LemonadeNexusClient& client,
                     BoringtunMesh& mesh,
                     const std::string& node_id);
    ~MeshOrchestrator();

    // Non-copyable, non-movable
    MeshOrchestrator(const MeshOrchestrator&) = delete;
    MeshOrchestrator& operator=(const MeshOrchestrator&) = delete;

    /// Start the orchestration loop with the given config.
    void start(const MeshConfig& config);

    /// Stop the orchestration loop and clean up mesh peers.
    void stop();

    /// Whether the orchestrator is running.
    [[nodiscard]] bool is_running() const;

    /// Get current mesh status (thread-safe snapshot).
    [[nodiscard]] MeshTunnelStatus status() const;

    /// Get current mesh peers (thread-safe snapshot).
    [[nodiscard]] std::vector<MeshPeer> peers() const;

    /// Wake the loop and run a peer refresh now, without waiting for the next
    /// refresh interval. Thread-safe. Has no effect when the loop is stopped.
    void refresh_now();

    /// Set callback for mesh state changes.
    void set_callback(StateCallback cb);

private:
    /// Main orchestration loop (runs on background thread).
    void run_loop();

    /// Fetch peers from server and sync to the mesh dataplane.
    void do_peer_refresh();

    /// Send heartbeat to server with our current endpoint.
    void do_heartbeat();

    /// Learn our reflexive address from the session server's STUN service.
    void do_stun_refresh();

    /// Run one hole-punch pass. Returns when the next probe is due.
    std::chrono::steady_clock::time_point do_punch();

    /// Poll mesh dataplane status and fire callback if changed.
    void do_monitor();

    /// Run one relay fallback pass over the known peers.
    void do_relay_maintenance();

    /// The relays the client may use: verified Tier 1 servers and listed relays.
    std::vector<RelayCandidate> relay_candidates();

    /// Start a certificate renewal thread when a registered certificate is
    /// due and no renewal thread is running.
    void maybe_start_cert_renewal();

    /// Interval between certificate checks. The check reads local files only.
    static constexpr std::chrono::seconds kCertCheckInterval{300};

    /// Total wait for one STUN query, including its resends.
    static constexpr std::chrono::milliseconds kStunTimeout{1500};

    LemonadeNexusClient&     client_;
    BoringtunMesh&           mesh_;
    std::string              node_id_;

    MeshConfig               config_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;

    mutable std::mutex       mutex_;
    std::condition_variable  cv_;          // for waking the loop early
    std::vector<MeshPeer>    known_peers_;
    std::map<std::string, uint32_t> stale_passes_;  // mesh key -> PeerLiveness::stale_passes
    MeshTunnelStatus         last_status_;
    StateCallback            callback_;
    std::string              our_endpoint_; // reflexive endpoint we publish ("" until learned)
    bool                     refresh_requested_{false};  // set by refresh_now()
    std::thread              cert_thread_;               // runs one renewal pass
    std::atomic<bool>        cert_busy_{false};          // cert_thread_ still working
    std::unique_ptr<RelayFallback> relay_fallback_;      // used by the loop thread only
    std::unique_ptr<HolePuncher>   hole_puncher_;        // used by the loop thread only
};

} // namespace lnsdk
