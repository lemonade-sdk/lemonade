#pragma once

/// Relay fallback for mesh peers that the BoringTun dataplane cannot reach
/// directly.
///
/// For each authorized peer, a maintenance pass decides whether the peer
/// needs a relay: it has no direct endpoint, or its direct endpoint gave no
/// handshake within the direct grace period. Both peers make that decision on
/// their own and pick the same relay (select_relay_for_pair). Each side then
/// requests a relay ticket from its verified Tier 1 server, derives the
/// shared rendezvous with BoringtunMesh::relay_rendezvous, and opens a
/// RelayChannel. When the relay pairs the two sides, the peer's dataplane
/// endpoint moves to the channel's local port. A channel that fails, or that
/// gives no handshake, is closed, and the peer is retried with exponential
/// backoff. A relay that refused, never answered, or went silent is skipped
/// for a cooldown period, so both sides move to the next relay.
///
/// With `prefer_direct`, HolePuncher keeps probing a relayed peer's listed
/// endpoint. When the peer answers there with an authenticated handshake, the
/// dataplane moves the peer to that address, and the next pass closes the
/// relay channel without counting a failure.
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

class RelayFallback {
public:
    /// Requests a relay ticket for this device's session, naming `relay_id`,
    /// from a verified Tier 1 server.
    using TicketSource = std::function<Result<RelayTicket>(const std::string& relay_id)>;
    /// Lists the relays the client may use.
    using CandidateSource = std::function<std::vector<RelayCandidate>()>;

    struct Timing {
        /// How long a direct endpoint may go without a handshake before the
        /// peer falls back to a relay.
        std::chrono::milliseconds direct_grace{20000};
        /// How long a paired relay may go without a handshake before it is
        /// abandoned.
        std::chrono::milliseconds handshake_grace{30000};
        /// A handshake younger than this counts as a working path, in seconds.
        int64_t handshake_fresh_sec{180};
        /// First retry delay after a failure; doubles per failure up to
        /// backoff_max.
        std::chrono::milliseconds backoff_base{5000};
        std::chrono::milliseconds backoff_max{120000};
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

    RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates);
    RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates,
                  Timing timing);
    /// Calls stop().
    ~RelayFallback();

    RelayFallback(const RelayFallback&) = delete;
    RelayFallback& operator=(const RelayFallback&) = delete;

    /// One maintenance pass over the current authorized peer list. With
    /// `prefer_direct` false, every peer uses a relay when one is available.
    void maintain(const std::vector<MeshPeer>& peers, bool prefer_direct);

    /// Close every channel and move every relayed peer off its relay.
    void stop();

    /// Relayed peers: mesh public key -> relay endpoint ("ip:port").
    /// Thread-safe.
    [[nodiscard]] std::map<std::string, std::string> relayed_peers() const;

    /// Whether a peer without an open relay channel needs one now. Pure.
    /// @param has_direct      the server lists a direct endpoint for the peer
    /// @param last_handshake  Unix seconds of the peer's last handshake, 0 if none
    /// @param now_unix        Unix seconds now
    /// @param known_for       how long this device has known the peer
    [[nodiscard]] static bool relay_needed(bool has_direct, int64_t last_handshake,
                                           int64_t now_unix, std::chrono::milliseconds known_for,
                                           bool prefer_direct, const Timing& timing);

private:
    using Clock = std::chrono::steady_clock;

    struct PeerState {
        std::unique_ptr<RelayChannel> channel;
        std::string                   relay_id;
        Clock::time_point             first_seen{};
        Clock::time_point             next_attempt{};
        Clock::time_point             paired_at{};
        int64_t                       paired_unix{0};     ///< Unix seconds of pairing
        int64_t                       abandoned_unix{0};  ///< Unix seconds of the last abandon
        uint32_t                      failures{0};
        bool                          applied{false};
    };

    /// Open a channel to the chosen relay for `peer`. False on failure.
    bool open_channel(PeerState& state, const MeshPeer& peer, const RelayCandidate& relay);
    /// Close the peer's channel, undo its relay path, and schedule a retry.
    void abandon(const std::string& pubkey, PeerState& state, bool blame_relay);
    /// Close the peer's channel because its traffic moved to a direct path,
    /// keep that path, and do not count a failure.
    void promote_direct(const std::string& pubkey, PeerState& state,
                        const std::string& listed_endpoint);
    void schedule_retry(PeerState& state);
    std::vector<RelayCandidate> usable_candidates(Clock::time_point now);

    BoringtunMesh&  mesh_;
    TicketSource    tickets_;
    CandidateSource candidates_;
    Timing          timing_;

    std::map<std::string, PeerState>         peers_;     // by mesh public key
    std::map<std::string, Clock::time_point> cooldown_;  // relay_id -> skip until
    std::vector<RelayCandidate>              candidate_cache_;
    Clock::time_point                        candidates_at_{};

    mutable std::mutex                       relayed_mtx_;
    std::map<std::string, std::string>       relayed_;   // pubkey -> relay endpoint
};

}  // namespace lnsdk
