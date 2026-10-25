#include <LemonadeNexusSDK/MeshOrchestrator.hpp>
#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>
#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <LemonadeNexusSDK/RelayChannel.hpp>

#include <spdlog/spdlog.h>

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
        last_status_ = {};
        our_endpoint_.clear();
    }

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

void MeshOrchestrator::run_loop() {
    using clock = std::chrono::steady_clock;

    auto next_peer_refresh = clock::now();
    auto next_stun         = clock::now();
    auto next_heartbeat    = clock::now();
    auto next_punch        = clock::now();
    auto next_monitor      = clock::now();
    auto next_cert_check   = clock::now();

    while (running_.load()) {
        auto now = clock::now();

        MeshConfig cfg;
        bool refresh_requested = false;
        {
            std::lock_guard lock(mutex_);
            cfg = config_;
            refresh_requested = std::exchange(refresh_requested_, false);
        }

        // Reflexive address, before the heartbeat that publishes it
        if (cfg.stun_refresh_interval_sec == 0) {
            next_stun = now + std::chrono::hours(24);
        } else if (now >= next_stun) {
            do_stun_refresh();
            next_stun = now + std::chrono::seconds(cfg.stun_refresh_interval_sec);
        }

        // Peer refresh
        if (refresh_requested || now >= next_peer_refresh) {
            do_peer_refresh();
            next_peer_refresh = now + std::chrono::seconds(cfg.peer_refresh_interval_sec);
        }

        // Heartbeat
        if (now >= next_heartbeat) {
            do_heartbeat();
            next_heartbeat = now + std::chrono::seconds(cfg.heartbeat_interval_sec);
        }

        // Direct paths
        if (now >= next_punch) {
            next_punch = do_punch();
        }

        // Monitor, then move unreachable peers onto a relay
        if (now >= next_monitor) {
            do_relay_maintenance();
            do_monitor();
            next_monitor = now + std::chrono::seconds(5); // 5s monitor interval
        }

        // Certificate renewal
        if (now >= next_cert_check) {
            maybe_start_cert_renewal();
            next_cert_check = now + kCertCheckInterval;
        }

        // Sleep until next event or woken early
        auto next_wake = std::min({next_peer_refresh, next_stun, next_heartbeat, next_punch,
                                   next_monitor, next_cert_check});
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

void MeshOrchestrator::do_peer_refresh() {
    // Fetch peers from server via HTTP
    auto result = client_.fetch_mesh_peers(node_id_);
    if (!result) {
        spdlog::warn("[MeshOrchestrator] peer refresh failed: {}", result.error);
        return;
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
    {
        std::lock_guard lock(mutex_);
        known_peers_ = std::move(server_peers);
        peer_count = known_peers_.size();
    }

    spdlog::debug("[MeshOrchestrator] peer refresh: {} peers synced", peer_count);
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
    }
}

void MeshOrchestrator::do_stun_refresh() {
    // The STUN server is the session server: the host of the dataplane's
    // server peer. The query leaves from the dataplane socket, so the answer
    // is the mapping that mesh peers can reach.
    const auto server = mesh_.server_endpoint();
    if (server.empty()) return;
    const auto stun_ip = resolve_relay_ipv4(split_endpoint(server).first);
    if (!stun_ip) {
        spdlog::debug("[MeshOrchestrator] STUN server {} does not resolve", server);
        return;
    }
    const auto reflexive =
        mesh_.query_reflexive(*stun_ip + ":" + std::to_string(kStunPort), kStunTimeout);
    if (!reflexive) {
        spdlog::debug("[MeshOrchestrator] no STUN answer from {}", *stun_ip);
        return;
    }
    // STUN is not authenticated: publish only an address that the STUN
    // server could have observed.
    if (!reflexive_publishable(*reflexive, *stun_ip)) {
        spdlog::warn("[MeshOrchestrator] STUN reported unusable address '{}'; not published",
                     *reflexive);
        return;
    }
    std::lock_guard lock(mutex_);
    if (our_endpoint_ != *reflexive) {
        spdlog::info("[MeshOrchestrator] reflexive endpoint {}", *reflexive);
    }
    our_endpoint_ = *reflexive;
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

void MeshOrchestrator::do_relay_maintenance() {
    std::vector<MeshPeer> peers;
    bool prefer_direct = true;
    {
        std::lock_guard lock(mutex_);
        peers = known_peers_;
        prefer_direct = config_.prefer_direct;
    }
    try {
        relay_fallback_->maintain(peers, prefer_direct);
    } catch (const std::exception& e) {
        spdlog::warn("[MeshOrchestrator] relay fallback pass failed: {}", e.what());
    }
}

void MeshOrchestrator::do_monitor() {
    auto ms = mesh_.mesh_status();

    // Merge boringtun handshake timestamps with server-reported last_seen
    // to get the most accurate peer liveness signal.
    auto now_epoch = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    constexpr int64_t kStaleThreshold = 15;  // 3 missed 5s keepalives

    {
        std::lock_guard lock(mutex_);
        for (auto& peer : known_peers_) {
            // Merge handshake timestamps from the dataplane status. The
            // dataplane knows a peer only by its mesh public key: its
            // tunnel_ip has no prefix and its node_id is empty.
            for (const auto& tp : ms.peers) {
                if (!peer.mesh_pubkey.empty() && tp.mesh_pubkey == peer.mesh_pubkey) {
                    if (tp.last_handshake > peer.last_handshake) {
                        peer.last_handshake = tp.last_handshake;
                    }
                    // A relayed peer reports the relay it goes through.
                    if (!tp.relay_endpoint.empty()) peer.relay_endpoint = tp.relay_endpoint;
                    break;
                }
            }

            // Online = best signal (Noise handshake or server last_seen) is fresh
            int64_t best_ts = std::max(peer.last_handshake,
                                        static_cast<int64_t>(peer.last_seen));
            if (best_ts > 0) {
                peer.is_online = (now_epoch - best_ts) < kStaleThreshold;
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
