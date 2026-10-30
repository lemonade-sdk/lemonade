#pragma once

#include <LemonadeNexusSDK/HolePunch.hpp>
#include <LemonadeNexusSDK/LanPaths.hpp>
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

/// The path a peer's traffic takes now (MeshPeer::path). Pure.
/// @param dataplane_endpoint  the address the dataplane sends to now
/// @param relay_endpoint      the relay the dataplane sends through ("" none)
/// @param fresh               peer_signal_fresh found the signal fresh
/// "relay" when the peer goes through a relay. Otherwise "" while the signal
/// is not fresh or no handshake or packet ever came; "lan" when the
/// dataplane sends to one of peer.local_endpoints; "direct" for any other
/// address; "" without one.
[[nodiscard]] std::string peer_path(const MeshPeer& peer, const std::string& dataplane_endpoint,
                                    const std::string& relay_endpoint, bool fresh);

/// Whether a peer lost its LAN path between two monitor passes: the earlier
/// pass found "lan" and the later one found no working path (""). Pure.
[[nodiscard]] bool lan_path_lost(const std::string& previous_path, const std::string& path);

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

/// Interval of the orchestrator's monitor pass (liveness and relay checks).
inline constexpr std::chrono::seconds kMonitorInterval{5};

/// A run-loop gap longer than this, beyond the pass's own work and planned
/// sleep, means the process was suspended (two monitor intervals).
inline constexpr std::chrono::milliseconds kResumeGap{2 * kMonitorInterval};

/// Whether the process was suspended between two run-loop passes. Pure.
/// @param wall_elapsed  wall-clock time between the starts of the two passes
/// @param busy          steady-clock time the first pass spent working
/// @param planned_wait  the sleep the first pass then asked for
/// The wall clock keeps running while a process is suspended, and the steady
/// clock may stop with it (Linux CLOCK_MONOTONIC during system suspend), so
/// neither the steady clock nor the loop's timers notice the gap. A wall
/// clock step forward also reads as a suspension; the resume path it starts
/// is safe to run at any time.
[[nodiscard]] bool loop_was_suspended(std::chrono::milliseconds wall_elapsed,
                                      std::chrono::milliseconds busy,
                                      std::chrono::milliseconds planned_wait);

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
///      The heartbeat also reports this device's LAN endpoints
///      (select_local_endpoints over list_interface_addresses, with the
///      dataplane port), read again every kLanRefreshInterval and in every
///      pass that an event below starts; a changed list goes out in a
///      heartbeat at once.
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
/// Some events make a pass run steps 1, 3, 5 and 7 at once instead of on
/// their intervals, with a new punch burst for every peer still being probed:
///   - the first pass after start() (join and enable_mesh),
///   - a refresh request (refresh_now(), or a heartbeat answer whose
///     peers_revision differs from the previous answer's),
///   - a resume: the wall clock shows a gap larger than kResumeGap between
///     passes (loop_was_suspended). The pass also sends a fresh handshake
///     initiation to every peer (BoringtunMesh::rehandshake_all) and the
///     heartbeat, and treats every peer's path as possibly changed.
/// A peer refresh that finds a known peer with a new listed endpoint, or a
/// peer that this device reads offline while the server saw it again, renews
/// that peer: it gets a punch burst at once, a handshake from its old path
/// no longer counts, and it may get a relay without the direct grace period.
/// A relay pairs when a direct endpoint gives no handshake within
/// kDirectAnswerWindow; the punch goes on, and a direct path replaces the
/// relay when it answers.
///
/// LAN-direct paths: a peer refresh keeps a peer's listed LAN endpoints only
/// when they pass lan_endpoint_usable and the peer is on this device's
/// network (lan_probe_targets: the same public address, or a LAN endpoint
/// inside one of this device's subnets). Such a peer is probed at its LAN
/// endpoints together with its listed endpoint, and gets no relay before
/// kLanAnswerWindow passed. A new LAN endpoint list renews the peer like a
/// new listed endpoint. Each monitor pass records the peer's path
/// (peer_path). A peer whose LAN path stops answering (no authenticated
/// packet within the liveness window of peer_signal_fresh) is renewed at
/// that pass: it gets a new punch burst, and a relay after the LAN window
/// when no path answers. A relayed peer keeps getting upgrade probes at its
/// LAN endpoints, so the LAN path returns when it answers again.
///
/// Peer endpoints come only from the verified server's peer list. Listed
/// endpoints that fail peer_endpoint_usable are dropped before any use. A
/// peers_revision only advances the next peer list fetch; it adds no peer.
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

    /// Wake the loop and run a pass now: peer refresh, STUN, a new punch
    /// burst for every peer still being probed, and relay fallback, without
    /// waiting for their intervals. Thread-safe. Has no effect when the loop
    /// is stopped.
    void refresh_now();

    /// Set callback for mesh state changes.
    void set_callback(StateCallback cb);

    /// The wall clock the loop reads to detect a suspension.
    using WallClock = std::function<std::chrono::system_clock::time_point()>;

    /// Replace the wall clock (tests simulate a suspension with it). Call
    /// before start(). An empty function restores the system clock.
    void set_wall_clock(WallClock clock);

    /// The source of this host's interface addresses.
    using LocalAddressSource = std::function<std::vector<InterfaceAddress>()>;

    /// Replace the interface list the LAN endpoints come from (tests use
    /// fixed addresses). Call before start(). An empty function restores
    /// list_interface_addresses().
    void set_local_address_source(LocalAddressSource source);

    /// The LAN endpoints this device reports now (thread-safe snapshot).
    [[nodiscard]] std::vector<std::string> local_endpoints() const;

private:
    /// Main orchestration loop (runs on background thread).
    void run_loop();

    /// Fetch peers from server and sync to the mesh dataplane. Returns the
    /// mesh keys of known peers whose path may have changed: a new listed
    /// endpoint, or offline here while the server saw them again.
    std::vector<std::string> do_peer_refresh();

    /// Send heartbeat to server with our current endpoint. A peers_revision
    /// that differs from the previous answer's requests a peer refresh.
    void do_heartbeat();

    /// Learn our reflexive address from the session server's STUN service.
    /// Returns true when the address changed.
    bool do_stun_refresh();

    /// Read this host's LAN addresses again and choose the LAN endpoints to
    /// report. Returns true when the reported list changed.
    bool do_lan_refresh();

    /// Run one hole-punch pass. Returns when the next probe is due.
    std::chrono::steady_clock::time_point do_punch();

    /// Poll mesh dataplane status and fire callback if changed. With
    /// `count_stale` false (an extra pass between monitor intervals), a stale
    /// signal changes nothing, so a peer still needs kPeerOfflinePasses
    /// stale monitor intervals to read offline; a fresh one sets it online.
    void do_monitor(bool count_stale);

    /// Run one relay fallback pass over the known peers. Returns when the
    /// next pass is due.
    std::chrono::steady_clock::time_point do_relay_maintenance();

    /// The relays the client may use: verified Tier 1 servers and listed relays.
    std::vector<RelayCandidate> relay_candidates();

    /// Start a certificate renewal thread when a registered certificate is
    /// due and no renewal thread is running.
    void maybe_start_cert_renewal();

    /// Interval between certificate checks. The check reads local files only.
    static constexpr std::chrono::seconds kCertCheckInterval{300};

    /// Interval between reads of the host's LAN addresses outside the
    /// passes that read them at once. The read is local and cheap.
    static constexpr std::chrono::seconds kLanRefreshInterval{60};

    /// Total wait for one STUN query, including its resends.
    static constexpr std::chrono::milliseconds kStunTimeout{1500};

    /// How long one peer list long-poll asks the server to hold, in seconds
    /// (the server's default and below its 30 s limit).
    static constexpr uint32_t kPeerWaitSec = 25;

    /// Pause after a failed long-poll: doubles from the first value to the
    /// second. A server that holds too many waits (429) gets the second at
    /// once, and one that does not hold waits at all gets kPeerWaitUnsupported.
    static constexpr std::chrono::seconds kPeerWaitRetryMin{1};
    static constexpr std::chrono::seconds kPeerWaitRetryMax{30};
    static constexpr std::chrono::seconds kPeerWaitUnsupported{300};

    /// Hold one peer list long-poll open to the session server and request a
    /// refresh pass as soon as the peers_revision changes. Runs on
    /// poll_thread_ while the orchestrator runs; the heartbeat revision check
    /// stays as the fallback.
    void poll_loop();

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
    std::vector<std::string> our_local_endpoints_;  // LAN endpoints we report
    std::vector<InterfaceAddress> own_lan_;         // our usable LAN interfaces
    LocalAddressSource       local_address_source_; // empty: list_interface_addresses
    bool                     refresh_requested_{false};  // set by refresh_now() and do_heartbeat()
    WallClock                wall_clock_;                // empty: the system clock
    std::string              peers_revision_;            // last known; guarded by mutex_
    std::thread              cert_thread_;               // runs one renewal pass
    std::thread              poll_thread_;               // runs poll_loop()
    std::atomic<bool>        cert_busy_{false};          // cert_thread_ still working
    std::unique_ptr<RelayFallback> relay_fallback_;      // used by the loop thread only
    std::unique_ptr<HolePuncher>   hole_puncher_;        // used by the loop thread only
};

} // namespace lnsdk
