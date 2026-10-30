#include <LemonadeNexusSDK/MeshOrchestrator.hpp>
#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>
#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <LemonadeNexusSDK/RelayChannel.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace {

/// The canonical relay port (README): every server relays on it.
constexpr uint16_t kRelayPort = 9103;
/// The canonical STUN port (README): every server answers STUN on it.
constexpr uint16_t kStunPort = 3478;

/// Split "host:port". Without exactly one colon, the whole text is the host
/// and the port is kRelayPort.
std::pair<std::string, uint16_t> split_endpoint(const std::string& endpoint) {
    std::string host = endpoint;
    uint16_t port = kRelayPort;
    const auto colon = endpoint.rfind(':');
    if (colon != std::string::npos && endpoint.find(':') == colon) {
        host = endpoint.substr(0, colon);
        try {
            const int parsed = std::stoi(endpoint.substr(colon + 1));
            if (parsed > 0 && parsed <= 65535) port = static_cast<uint16_t>(parsed);
        } catch (...) {
        }
    }
    return {host, port};
}

}  // namespace

namespace lnsdk {

std::optional<bool> peer_signal_fresh(const MeshPeer& peer, int64_t now_unix,
                                      uint32_t peer_refresh_interval_sec) {
    // Keepalives do not renew the handshake, and an idle boringtun session
    // lives until REJECT_AFTER_TIME (180 s) before it renegotiates, so the
    // packets themselves are the liveness signal once one arrived. The
    // server's last_seen arrives only with the peer list, so it may be one
    // peer refresh old.
    constexpr int64_t kSessionLive    = 200;  // REJECT_AFTER_TIME 180 s, a retry, jitter
    constexpr int64_t kHeartbeatStale = 15;   // 3 missed 5 s heartbeats
    constexpr int64_t kMonitorTick    = 5;
    const auto last_seen = static_cast<int64_t>(peer.last_seen);
    const bool seen_recently =
        last_seen > 0 &&
        now_unix - last_seen < kHeartbeatStale + static_cast<int64_t>(peer_refresh_interval_sec);
    if (peer.last_rx > 0) {
        const int64_t rx_stale = 3 * std::max<int64_t>(peer.keepalive, 1) + kMonitorTick;
        return now_unix - peer.last_rx < rx_stale ||
               (!peer.relay_endpoint.empty() && seen_recently);
    }
    if (peer.last_handshake > 0 || last_seen > 0) {
        const bool session_live =
            peer.last_handshake > 0 && now_unix - peer.last_handshake < kSessionLive;
        return session_live || seen_recently;
    }
    return std::nullopt;
}

bool loop_was_suspended(std::chrono::milliseconds wall_elapsed, std::chrono::milliseconds busy,
                        std::chrono::milliseconds planned_wait) {
    return wall_elapsed - busy - std::max(planned_wait, std::chrono::milliseconds{0}) > kResumeGap;
}

PeerLiveness next_peer_liveness(PeerLiveness previous, bool fresh) {
    if (fresh) return {true, 0};
    if (previous.online && previous.stale_passes + 1 < kPeerOfflinePasses) {
        return {true, previous.stale_passes + 1};
    }
    return {false, 0};
}

MeshOrchestrator::MeshOrchestrator(LemonadeNexusClient& client,
                                   BoringtunMesh& mesh,
                                   const std::string& node_id)
    : client_(client), mesh_(mesh), node_id_(node_id) {
    relay_fallback_ = std::make_unique<RelayFallback>(
        mesh_,
        // The ticket names this device's own session, and only the session's
        // verified Tier 1 server issues it (private API).
        [this](const std::string& relay_id) {
            return client_.request_relay_ticket(node_id_, relay_id);
        },
        [this] { return relay_candidates(); });
    hole_puncher_ = std::make_unique<HolePuncher>(mesh_);
}

MeshOrchestrator::~MeshOrchestrator() {
    stop();
}

void MeshOrchestrator::start(const MeshConfig& config) {
    if (running_.load()) return;

    {
        std::lock_guard lock(mutex_);
        config_ = config;
    }

    running_ = true;
    thread_ = std::thread([this] { run_loop(); });

    spdlog::info("[MeshOrchestrator] started for node {}", node_id_);
}

void MeshOrchestrator::stop() {
    if (!running_.exchange(false)) return;

    // Take the lock so the loop cannot miss the change between its predicate
    // check and its wait.
    { std::lock_guard lock(mutex_); }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // A renewal in progress finishes its request (bounded by the HTTP
    // timeouts); its callback must not run after stop() returns.
    if (cert_thread_.joinable()) cert_thread_.join();

    // The loop thread has ended, so the relay fallback and the hole puncher
    // have no other user.
    relay_fallback_->stop();
    hole_puncher_->reset();

    // Remove all mesh peers from the dataplane
    {
        std::lock_guard lock(mutex_);
        for (const auto& peer : known_peers_) {
            mesh_.remove_peer(peer.mesh_pubkey);
        }
        known_peers_.clear();
        stale_passes_.clear();
        last_status_ = {};
        our_endpoint_.clear();
    }
    peers_revision_.clear();

    spdlog::info("[MeshOrchestrator] stopped for node {}", node_id_);
}

bool MeshOrchestrator::is_running() const {
    return running_.load();
}

MeshTunnelStatus MeshOrchestrator::status() const {
    std::lock_guard lock(mutex_);
    return last_status_;
}

std::vector<MeshPeer> MeshOrchestrator::peers() const {
    std::lock_guard lock(mutex_);
    return known_peers_;
}

void MeshOrchestrator::refresh_now() {
    {
        std::lock_guard lock(mutex_);
        refresh_requested_ = true;
    }
    cv_.notify_all();
}

void MeshOrchestrator::set_callback(StateCallback cb) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(cb);
}

void MeshOrchestrator::set_wall_clock(WallClock clock) {
    std::lock_guard lock(mutex_);
    wall_clock_ = std::move(clock);
}

void MeshOrchestrator::run_loop() {
    using clock = std::chrono::steady_clock;
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;

    auto next_peer_refresh = clock::now();
    auto next_stun         = clock::now();
    auto next_heartbeat    = clock::now();
    auto next_punch        = clock::now();
    auto next_relay        = clock::now();
    auto next_monitor      = clock::now();
    auto next_cert_check   = clock::now();

    std::optional<std::chrono::system_clock::time_point> last_wall;
    milliseconds busy{0};
    milliseconds planned_wait{0};

    while (running_.load()) {
        auto now = clock::now();

        MeshConfig cfg;
        bool refresh_requested = false;
        std::chrono::system_clock::time_point wall_now;
        {
            std::lock_guard lock(mutex_);
            cfg = config_;
            refresh_requested = std::exchange(refresh_requested_, false);
            wall_now = wall_clock_ ? wall_clock_() : std::chrono::system_clock::now();
        }

        // A suspended process (an app in the background, a sleeping laptop)
        // finds its sessions, NAT mappings, and reflexive address possibly
        // gone, and its peers may have moved meanwhile.
        const bool resumed =
            last_wall &&
            loop_was_suspended(duration_cast<milliseconds>(wall_now - *last_wall), busy,
                               planned_wait);
        last_wall = wall_now;
        if (resumed) {
            const auto sent = mesh_.rehandshake_all();
            std::vector<std::string> all;
            {
                std::lock_guard lock(mutex_);
                for (const auto& peer : known_peers_) all.push_back(peer.mesh_pubkey);
            }
            hole_puncher_->renew(all);
            relay_fallback_->renew(all, false);
            spdlog::info("[MeshOrchestrator] resumed after a suspension: {} handshakes sent, "
                         "checking {} peer paths",
                         sent, all.size());
        }
        const bool urgent = refresh_requested || resumed;

        // Reflexive address, before the heartbeat that publishes it
        if (cfg.stun_refresh_interval_sec == 0) {
            next_stun = now + std::chrono::hours(24);
        } else if (urgent || now >= next_stun) {
            // A new address goes out at once: peers can punch only toward
            // the address the server lists.
            if (do_stun_refresh()) next_heartbeat = now;
            next_stun = now + std::chrono::seconds(cfg.stun_refresh_interval_sec);
        }

        // Peer refresh
        bool refreshed = false;
        if (urgent || now >= next_peer_refresh) {
            const auto renewed = do_peer_refresh();
            if (!renewed.empty()) {
                hole_puncher_->renew(renewed);
                relay_fallback_->renew(renewed, true);
            }
            next_peer_refresh = now + std::chrono::seconds(cfg.peer_refresh_interval_sec);
            refreshed = true;
        }

        // Heartbeat
        if (resumed || now >= next_heartbeat) {
            do_heartbeat();
            next_heartbeat = now + std::chrono::seconds(cfg.heartbeat_interval_sec);
        }

        // Direct paths. A refreshed peer list may hold new or renewed peers:
        // punch them in this pass, not at the next probe time.
        bool paths_checked = false;
        if (urgent) hole_puncher_->burst_now();
        if (urgent || refreshed || now >= next_punch) {
            next_punch = do_punch();
            paths_checked = true;
        }

        // Relays, in the same pass, so a peer without a direct path gets one
        // as soon as its direct grace period ends.
        if (urgent || refreshed || now >= next_relay || now >= next_monitor) {
            next_relay = do_relay_maintenance();
            paths_checked = true;
        }

        // Liveness. Between monitor intervals, a pass that touched the paths
        // reports a peer that came online without waiting for the interval.
        if (now >= next_monitor) {
            do_monitor(true);
            next_monitor = now + kMonitorInterval;
        } else if (paths_checked) {
            do_monitor(false);
        }

        // Certificate renewal
        if (now >= next_cert_check) {
            maybe_start_cert_renewal();
            next_cert_check = now + kCertCheckInterval;
        }

        // Sleep until next event or woken early
        auto next_wake = std::min({next_peer_refresh, next_stun, next_heartbeat, next_punch,
                                   next_relay, next_monitor, next_cert_check});
        const auto pass_end = clock::now();
        busy = duration_cast<milliseconds>(pass_end - now);
        planned_wait = std::max(milliseconds{0}, duration_cast<milliseconds>(next_wake - pass_end));
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next_wake,
                       [this] { return !running_.load() || refresh_requested_; });
    }
}

void MeshOrchestrator::maybe_start_cert_renewal() {
    if (cert_busy_.load()) return;
    try {
        if (!client_.certificate_renewal_pending()) return;
        if (cert_thread_.joinable()) cert_thread_.join();  // the previous pass ended
        cert_busy_ = true;
        cert_thread_ = std::thread([this] {
            try {
                client_.renew_managed_certificates();
            } catch (const std::exception& e) {
                spdlog::warn("[MeshOrchestrator] certificate renewal failed: {}", e.what());
            } catch (...) {
                spdlog::warn("[MeshOrchestrator] certificate renewal failed");
            }
            cert_busy_ = false;
        });
    } catch (const std::exception& e) {
        cert_busy_ = false;
        spdlog::warn("[MeshOrchestrator] cannot start certificate renewal: {}", e.what());
    }
}

std::vector<std::string> MeshOrchestrator::do_peer_refresh() {
    // Fetch peers from server via HTTP
    auto result = client_.fetch_mesh_peers(node_id_);
    if (!result) {
        spdlog::warn("[MeshOrchestrator] peer refresh failed: {}", result.error);
        return {};
    }

    auto server_peers = std::move(result.value);

    // A listed endpoint is only a send target: drop one that is not a usable
    // unicast address before the dataplane or the hole puncher sees it.
    const auto server_scope = endpoint_scope(mesh_.server_endpoint());
    for (auto& peer : server_peers) {
        for (auto* endpoint : {&peer.endpoint, &peer.relay_endpoint}) {
            if (!endpoint->empty() && !peer_endpoint_usable(*endpoint, server_scope)) {
                spdlog::warn("[MeshOrchestrator] peer {} lists unusable endpoint '{}'; ignored",
                             peer.node_id, *endpoint);
                endpoint->clear();
            }
        }
    }

    // Sync to the boringtun mesh dataplane
    auto sync_result = mesh_.sync_peers(server_peers);
    if (!sync_result) {
        spdlog::warn("[MeshOrchestrator] sync_peers failed: {}", sync_result.error);
    }

    size_t peer_count;
    std::vector<std::string> renewed;
    {
        std::lock_guard lock(mutex_);
        // Keep what do_monitor worked out for peers already known: the
        // server's list carries no dataplane timestamps, so a fresh copy read
        // "offline" until the next monitor pass, every refresh.
        for (auto& peer : server_peers) {
            for (const auto& old : known_peers_) {
                if (old.node_id != peer.node_id || old.mesh_pubkey != peer.mesh_pubkey) continue;
                // The peer moved (a new reflexive address, another network,
                // a rejoin), or it is offline here while the server saw it
                // again: the old path may be gone, and the peer's side is
                // probing toward this device now.
                const bool moved = !peer.endpoint.empty() && peer.endpoint != old.endpoint;
                const bool back = !old.is_online && peer.last_seen > old.last_seen;
                if (moved || back) {
                    renewed.push_back(peer.mesh_pubkey);
                    spdlog::info("[MeshOrchestrator] peer {} {}; probing it now", peer.node_id,
                                 moved ? "lists a new endpoint" : "is back online at the server");
                }
                peer.last_handshake = std::max(peer.last_handshake, old.last_handshake);
                peer.last_rx        = std::max(peer.last_rx, old.last_rx);
                peer.is_online      = old.is_online;
                // The server does not know the relay path; the next monitor
                // pass corrects it. Without this the path read "" between a
                // refresh and that pass, and the mesh port saw a path change.
                if (peer.relay_endpoint.empty()) peer.relay_endpoint = old.relay_endpoint;
                break;
            }
        }
        known_peers_ = std::move(server_peers);
        peer_count = known_peers_.size();
    }

    spdlog::debug("[MeshOrchestrator] peer refresh: {} peers synced", peer_count);
    return renewed;
}

void MeshOrchestrator::do_heartbeat() {
    // Report our reflexive endpoint so authorized peers can punch toward it.
    // Until STUN gives one, the endpoint is empty and the server keeps what
    // it stored before.
    std::string endpoint;
    {
        std::lock_guard lock(mutex_);
        endpoint = our_endpoint_;
    }

    auto result = client_.mesh_heartbeat(node_id_, endpoint);
    if (!result) {
        spdlog::debug("[MeshOrchestrator] heartbeat failed: {}", result.error);
        return;
    }
    // A server without peers_revision leaves the peer refresh interval as
    // the only schedule. The first value is the baseline: the start pass
    // has just fetched the list.
    if (result.value.empty()) return;
    if (!peers_revision_.empty() && result.value != peers_revision_) {
        spdlog::debug("[MeshOrchestrator] peer list changed at the server; refreshing");
        std::lock_guard lock(mutex_);
        refresh_requested_ = true;
    }
    peers_revision_ = std::move(result.value);
}

bool MeshOrchestrator::do_stun_refresh() {
    // The STUN server is the session server: the host of the dataplane's
    // server peer. The query leaves from the dataplane socket, so the answer
    // is the mapping that mesh peers can reach.
    const auto server = mesh_.server_endpoint();
    if (server.empty()) return false;
    const auto stun_ip = resolve_relay_ipv4(split_endpoint(server).first);
    if (!stun_ip) {
        spdlog::debug("[MeshOrchestrator] STUN server {} does not resolve", server);
        return false;
    }
    const auto reflexive =
        mesh_.query_reflexive(*stun_ip + ":" + std::to_string(kStunPort), kStunTimeout);
    if (!reflexive) {
        spdlog::debug("[MeshOrchestrator] no STUN answer from {}", *stun_ip);
        return false;
    }
    // STUN is not authenticated: publish only an address that the STUN
    // server could have observed.
    if (!reflexive_publishable(*reflexive, *stun_ip)) {
        spdlog::warn("[MeshOrchestrator] STUN reported unusable address '{}'; not published",
                     *reflexive);
        return false;
    }
    std::lock_guard lock(mutex_);
    if (our_endpoint_ == *reflexive) return false;
    spdlog::info("[MeshOrchestrator] reflexive endpoint {}", *reflexive);
    our_endpoint_ = *reflexive;
    return true;
}

std::chrono::steady_clock::time_point MeshOrchestrator::do_punch() {
    std::vector<MeshPeer> peers;
    bool prefer_direct = true;
    {
        std::lock_guard lock(mutex_);
        peers = known_peers_;
        prefer_direct = config_.prefer_direct;
    }
    return hole_puncher_->maintain(peers, relay_fallback_->relayed_peers(), prefer_direct);
}

std::vector<RelayCandidate> MeshOrchestrator::relay_candidates() {
    std::vector<RelayCandidate> out;
    // Verified Tier 1 servers: their relay id is their node identity.
    for (const auto& server : client_.tier1_membership().servers) {
        out.push_back({server.node_id, server.host, kRelayPort});
    }
    // Relays the session's server lists. Tier 2 relays appear here.
    auto listed = client_.list_relays();
    if (listed) {
        for (const auto& relay : listed.value) {
            if (!relay.supports_relay || relay.relay_id.empty() || relay.endpoint.empty()) continue;
            auto [host, port] = split_endpoint(relay.endpoint);
            out.push_back({relay.relay_id, host, port});
        }
    }
    return out;
}

std::chrono::steady_clock::time_point MeshOrchestrator::do_relay_maintenance() {
    std::vector<MeshPeer> peers;
    bool prefer_direct = true;
    {
        std::lock_guard lock(mutex_);
        peers = known_peers_;
        prefer_direct = config_.prefer_direct;
    }
    try {
        return relay_fallback_->maintain(peers, prefer_direct);
    } catch (const std::exception& e) {
        spdlog::warn("[MeshOrchestrator] relay fallback pass failed: {}", e.what());
    }
    return std::chrono::steady_clock::now() + kMonitorInterval;
}

void MeshOrchestrator::do_monitor(bool count_stale) {
    auto ms = mesh_.mesh_status();

    auto now_epoch = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    {
        std::lock_guard lock(mutex_);
        std::map<std::string, uint32_t> stale_passes;
        for (auto& peer : known_peers_) {
            // Merge handshake timestamps from the dataplane status. The
            // dataplane knows a peer only by its mesh public key: its
            // tunnel_ip has no prefix and its node_id is empty.
            for (const auto& tp : ms.peers) {
                if (!peer.mesh_pubkey.empty() && tp.mesh_pubkey == peer.mesh_pubkey) {
                    if (tp.last_handshake > peer.last_handshake) {
                        peer.last_handshake = tp.last_handshake;
                    }
                    if (tp.last_rx > peer.last_rx) peer.last_rx = tp.last_rx;
                    // A relayed peer reports the relay it goes through. A
                    // peer the dataplane now reaches at another address left
                    // its relay; a listed relay node keeps its endpoint.
                    if (!tp.relay_endpoint.empty()) {
                        peer.relay_endpoint = tp.relay_endpoint;
                    } else if (!tp.endpoint.empty() && peer.relay_endpoint != tp.endpoint) {
                        peer.relay_endpoint.clear();
                    }
                    break;
                }
            }

            const auto fresh =
                peer_signal_fresh(peer, now_epoch, config_.peer_refresh_interval_sec);
            if (!fresh) continue;
            const auto found = stale_passes_.find(peer.mesh_pubkey);
            if (!count_stale && !*fresh) {
                if (found != stale_passes_.end()) stale_passes[peer.mesh_pubkey] = found->second;
                continue;
            }
            const auto next = next_peer_liveness(
                {peer.is_online, found == stale_passes_.end() ? 0u : found->second}, *fresh);
            peer.is_online = next.online;
            if (next.stale_passes > 0) stale_passes[peer.mesh_pubkey] = next.stale_passes;
        }
        stale_passes_ = std::move(stale_passes);

        // The dataplane knows its peers only by key: report them with the
        // node, name and liveness worked out above, as get_mesh_peers does.
        for (auto& tp : ms.peers) {
            for (const auto& peer : known_peers_) {
                if (peer.mesh_pubkey.empty() || peer.mesh_pubkey != tp.mesh_pubkey) continue;
                tp.node_id   = peer.node_id;
                tp.hostname  = peer.hostname;
                tp.is_online = peer.is_online;
                break;
            }
        }

        // Recompute online_count from local peer data
        ms.online_count = 0;
        for (const auto& p : known_peers_) {
            if (p.is_online) ++ms.online_count;
        }
        ms.peer_count = static_cast<uint32_t>(known_peers_.size());
    }

    StateCallback cb;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = (ms.peer_count   != last_status_.peer_count ||
                   ms.online_count != last_status_.online_count ||
                   ms.is_up        != last_status_.is_up);
        last_status_ = ms;
        cb = callback_;
    }

    if (changed && cb) {
        cb(ms);
    }
}

} // namespace lnsdk
