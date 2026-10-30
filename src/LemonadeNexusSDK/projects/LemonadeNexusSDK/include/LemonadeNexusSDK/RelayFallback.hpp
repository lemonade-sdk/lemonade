#pragma once

/// Relay fallback for mesh peers that the BoringTun dataplane cannot reach
/// directly.
///
/// For each authorized peer, a maintenance pass decides whether the peer
/// needs a relay: it has no direct endpoint, or its direct endpoint gave no
/// handshake within the direct grace period (kDirectAnswerWindow by
/// default). The grace period is short on purpose: the relay brings the peer
/// online at once, the punch goes on in the background, and a direct path
/// that answers later replaces the relay (prefer_direct). Both peers make
/// that decision on their own and pick the same relay
/// (select_relay_for_pair). Each side then
/// requests a relay ticket from its verified Tier 1 server, derives the
/// shared rendezvous with BoringtunMesh::relay_rendezvous, and opens a
/// RelayChannel. When the relay pairs the two sides, the peer's dataplane
/// endpoint moves to the channel's local port. A channel that fails, or that
/// gives no handshake, is closed, and the peer is retried with exponential
/// backoff. A relay that refused, never answered, or went silent is skipped
/// for a cooldown period, so both sides move to the next relay.
///
/// A peer on this device's network (MeshPeer::local_endpoints not empty)
/// gets no relay before its LAN probes had kLanAnswerWindow to answer,
/// counted from when this device first lists or renews the peer, also when a
/// renewal skips the direct grace period. A peer without LAN endpoints does
/// not wait for that window.
///
/// With `prefer_direct`, HolePuncher keeps probing a relayed peer's listed
/// endpoint and LAN endpoints. When the peer answers there with an authenticated handshake, the
/// dataplane moves the peer to that address, and the next pass closes the
/// relay channel without counting a failure. A direct handshake that arrives
/// before the relay path is in use closes the channel request the same way.
///
/// The rendezvous changes every kRelayRendezvousWindowSec (300 s) of wall
/// time, and the relay pairs only requests of the same window. A request
/// still waiting when its window ends is replaced by one for the new window
/// at once, so two sides that asked on either side of the boundary pair
/// within one pass instead of after their requests expire.
///
/// renew() restarts the decision for a peer whose path may have changed: a
/// handshake from before the renewal no longer proves a working path. A
/// working relay channel stays open.
///
/// Not thread-safe except relayed_peers(): call maintain() and stop() from
/// one thread (the mesh orchestrator's).

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lnsdk {

class BoringtunMesh;
class RelayChannel;

/// A relay the client may use. `relay_id` is the relay's base64 node identity
/// key, the id the relay ticket must name.
struct RelayCandidate {
    std::string relay_id;
    std::string host;        ///< DNS name or dotted IPv4 address
    uint16_t    port{9103};
};

/// How long a direct endpoint has to answer the first punch burst before
/// the peer is paired with a relay (RelayFallback::Timing::direct_grace).
/// The punch burst starts in the same orchestrator pass, so this is the time
/// from the first probe; a handshake on the LAN or through a cooperative NAT
/// completes well within it.
inline constexpr std::chrono::milliseconds kDirectAnswerWindow{3000};

/// How long the LAN endpoints of a peer on this device's network have to
/// answer before the peer may be paired with a relay
/// (RelayFallback::Timing::lan_grace). A LAN handshake takes milliseconds;
/// the window covers a few lost probes and a busy device.
inline constexpr std::chrono::milliseconds kLanAnswerWindow{1500};

class RelayFallback {
public:
    /// Requests a relay ticket for this device's session, naming `relay_id`,
    /// from a verified Tier 1 server.
    using TicketSource = std::function<Result<RelayTicket>(const std::string& relay_id)>;
    /// Lists the relays the client may use.
    using CandidateSource = std::function<std::vector<RelayCandidate>()>;

    struct Timing {
        /// How long a direct endpoint may go without a handshake before the
        /// peer falls back to a relay, counted from when this device first
        /// lists the peer or renews it.
        std::chrono::milliseconds direct_grace{kDirectAnswerWindow};
        /// How long a peer with LAN endpoints may go without a handshake
        /// before it may get a relay, counted from when this device first
        /// lists the peer or renews it (with or without skip_grace).
        std::chrono::milliseconds lan_grace{kLanAnswerWindow};
        /// How long a paired relay may go without carrying traffic (a
        /// handshake or an authenticated packet) before it is abandoned and
        /// blamed. A relay that carried traffic is abandoned only after
        /// handshake_fresh_sec plus this grace without any.
        std::chrono::milliseconds handshake_grace{30000};
        /// A handshake younger than this counts as a working path, in seconds.
        int64_t handshake_fresh_sec{180};
        /// First retry delay after a failure; doubles per failure up to
        /// backoff_max.
        std::chrono::milliseconds backoff_base{5000};
        std::chrono::milliseconds backoff_max{120000};
        /// Retry delay after a relay request that the peer did not join
        /// (the relay ends a waiting request after 30 s), within the eager
        /// period. Such a request is not a failure. Keeping a request waiting
        /// at the relay all the time lets the peer pair whenever it asks, on
        /// whatever schedule its own SDK version uses; with the backoff, the
        /// two sides' 30 s windows could miss each other for minutes.
        std::chrono::milliseconds absent_retry{1000};
        /// How long after this device first lists or renews a peer a request
        /// the peer did not join is repeated after absent_retry. Later, such a
        /// request counts as a failure and backs off, so a peer that is not
        /// running costs a request only every few minutes.
        std::chrono::milliseconds eager_period{300000};
        /// How long a failed relay is skipped.
        std::chrono::milliseconds relay_cooldown{300000};
        /// How long a candidate list is reused.
        std::chrono::milliseconds candidate_ttl{60000};
        /// Channel timing passed to every RelayChannel.
        std::chrono::milliseconds allocate_retry{1000};
        std::chrono::milliseconds heartbeat_interval{10000};
        std::chrono::milliseconds pending_limit{35000};
    };

    /// Most relay channels open at once, and most opened per pass.
    static constexpr std::size_t kMaxChannels = 64;
    static constexpr std::size_t kMaxOpensPerPass = 4;
    /// The next pass that maintain() reports: within kPendingPassGap while a
    /// channel waits for its pairing or its first traffic, and never later
    /// than kMaxPassGap.
    static constexpr std::chrono::milliseconds kMinPassGap{250};
    static constexpr std::chrono::milliseconds kPendingPassGap{500};
    static constexpr std::chrono::milliseconds kMaxPassGap{5000};

    RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates);
    RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates,
                  Timing timing);
    /// Calls stop().
    ~RelayFallback();

    RelayFallback(const RelayFallback&) = delete;
    RelayFallback& operator=(const RelayFallback&) = delete;

    /// One maintenance pass over the current authorized peer list. With
    /// `prefer_direct` false, every peer uses a relay when one is available.
    /// Returns the steady-clock time when the next pass is due, from
    /// kMinPassGap to kMaxPassGap ahead: sooner while a peer's direct grace
    /// period runs out or a channel is being set up.
    std::chrono::steady_clock::time_point maintain(const std::vector<MeshPeer>& peers,
                                                   bool prefer_direct);

    /// The paths to these peers (mesh public keys) may have changed: the
    /// server lists a new endpoint, the peer came back online, or this
    /// device resumed. A handshake from before this call no longer proves a
    /// working direct path. With `skip_grace`, a peer without a working path
    /// may get a relay at the next pass; otherwise its direct grace period
    /// starts again now. A pending retry delay ends, and the eager period
    /// starts again; the failure count that sets the next backoff stays. An open relay channel stays as it is: a
    /// relay that carries traffic is kept (paired_action).
    void renew(const std::vector<std::string>& pubkeys, bool skip_grace);

    /// Close every channel and move every relayed peer off its relay.
    void stop();

    /// Unix seconds now, as the rendezvous window is computed from it.
    using UnixClock = std::function<int64_t()>;

    /// Replace the clock that picks the rendezvous window (tests cross a
    /// window boundary with it). Call before the first maintain(). An empty
    /// function restores the system clock.
    void set_window_clock(UnixClock clock);

    /// Relayed peers: mesh public key -> relay endpoint ("ip:port").
    /// Thread-safe.
    [[nodiscard]] std::map<std::string, std::string> relayed_peers() const;

    /// Whether a peer without an open relay channel needs one now. Pure.
    /// @param has_direct      the server lists a direct endpoint for the peer
    /// @param last_handshake  Unix seconds of the peer's last handshake, 0 if none
    /// @param now_unix        Unix seconds now
    /// @param known_for       time since the peer's direct grace period
    ///                        started: this device first listed the peer,
    ///                        or renewed it
    /// @param has_lan         the peer has LAN endpoints to probe
    /// @param lan_known_for   time since this device first listed the peer
    ///                        or last renewed it (the LAN window's start)
    [[nodiscard]] static bool relay_needed(bool has_direct, int64_t last_handshake,
                                           int64_t now_unix, std::chrono::milliseconds known_for,
                                           bool prefer_direct, const Timing& timing,
                                           bool has_lan = false,
                                           std::chrono::milliseconds lan_known_for = {});

    /// What a maintenance pass does with a paired relay channel.
    enum class PairedAction {
        Wait,             ///< Paired, no traffic through it yet; within the grace period
        Working,          ///< The relay carried traffic recently
        PromoteDirect,    ///< A direct path answered; close the relay
        AbandonUnproven,  ///< No traffic within handshake_grace; blame the relay
        AbandonStale,     ///< Carried traffic once, none for handshake_fresh_sec + grace
    };

    /// The decision for a paired relay channel. Pure.
    /// @param relay_seen       Unix seconds of the newest handshake or
    ///                         authenticated packet that arrived through the
    ///                         relay since pairing, 0 if none. The dataplane
    ///                         reads no handshake while an idle session
    ///                         renegotiates after REJECT_AFTER_TIME, so this
    ///                         keeps the last one seen.
    /// @param handshake        Unix seconds of a handshake since pairing that
    ///                         the dataplane reports now, 0 if none
    /// @param paired_for       time since the channel paired
    /// @param direct_answered  prefer_direct is set, the peer has a direct or
    ///                         LAN endpoint, and the dataplane moved the peer
    ///                         to another address than the relay
    [[nodiscard]] static PairedAction paired_action(int64_t relay_seen, int64_t handshake,
                                                    int64_t now_unix,
                                                    std::chrono::milliseconds paired_for,
                                                    bool direct_answered, const Timing& timing);

private:
    using Clock = std::chrono::steady_clock;

    struct PeerState {
        std::unique_ptr<RelayChannel> channel;
        std::string                   relay_id;
        Clock::time_point             direct_since{};  ///< Start of the direct grace period
        Clock::time_point             eager_until{};   ///< End of the eager period
        Clock::time_point             lan_since{};     ///< Start of the LAN window
        Clock::time_point             next_attempt{};
        Clock::time_point             paired_at{};
        int64_t                       opened_unix{0};   ///< Unix seconds the channel opened
        uint64_t                      window{0};        ///< Rendezvous window of the request
        int64_t                       paired_unix{0};   ///< Unix seconds of pairing
        int64_t                       relay_seen{0};    ///< see paired_action()
        int64_t                       handshake_seen{0};  ///< Newest handshake reading
        /// Unix seconds of the last abandon or renewal: a handshake at or
        /// before it went through a path that is gone.
        int64_t                       stale_before{0};
        uint32_t                      failures{0};
        bool                          applied{false};
        bool                          listed{false};    ///< maintain() has seen the peer
    };

    /// Open a channel to the chosen relay for `peer`. False on failure.
    bool open_channel(PeerState& state, const MeshPeer& peer, const RelayCandidate& relay);
    /// Close the peer's channel, undo its relay path, and schedule a retry:
    /// after absent_retry when `peer_absent` (the peer never joined the
    /// request) and the eager period runs, otherwise with backoff.
    void abandon(const std::string& pubkey, PeerState& state, bool blame_relay,
                 bool peer_absent = false);
    /// Close the peer's channel because its traffic moved to a direct path,
    /// keep that path, and do not count a failure.
    void promote_direct(const std::string& pubkey, PeerState& state,
                        const std::string& listed_endpoint);
    /// Close a channel whose relay path is not in use yet because the peer
    /// answered on a direct path first. No failure, no backoff.
    void release_unused(const std::string& pubkey, PeerState& state);
    void schedule_retry(PeerState& state);
    std::vector<RelayCandidate> usable_candidates(Clock::time_point now);

    BoringtunMesh&  mesh_;
    TicketSource    tickets_;
    CandidateSource candidates_;
    Timing          timing_;

    std::map<std::string, PeerState>         peers_;     // by mesh public key
    std::map<std::string, Clock::time_point> cooldown_;  // relay_id -> skip until
    std::vector<RelayCandidate>              candidate_cache_;
    UnixClock                                window_clock_;  // empty: the system clock
    Clock::time_point                        candidates_at_{};

    mutable std::mutex                       relayed_mtx_;
    std::map<std::string, std::string>       relayed_;   // pubkey -> relay endpoint
};

}  // namespace lnsdk
