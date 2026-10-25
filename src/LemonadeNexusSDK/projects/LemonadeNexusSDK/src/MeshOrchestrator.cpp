#include <LemonadeNexusSDK/MeshOrchestrator.hpp>
#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>
#include <LemonadeNexusSDK/BoringtunMesh.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace lnsdk {

MeshOrchestrator::MeshOrchestrator(LemonadeNexusClient& client,
                                   BoringtunMesh& mesh,
                                   const std::string& node_id)
    : client_(client), mesh_(mesh), node_id_(node_id) {}

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

    // Remove all mesh peers from the dataplane
    {
        std::lock_guard lock(mutex_);
        for (const auto& peer : known_peers_) {
            mesh_.remove_peer(peer.mesh_pubkey);
        }
        known_peers_.clear();
        last_status_ = {};
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
    auto next_heartbeat    = clock::now();
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

        // Monitor
        if (now >= next_monitor) {
            do_monitor();
            next_monitor = now + std::chrono::seconds(5); // 5s monitor interval
        }

        // Certificate renewal
        if (now >= next_cert_check) {
            maybe_start_cert_renewal();
            next_cert_check = now + kCertCheckInterval;
        }

        // Sleep until next event or woken early
        auto next_wake = std::min({next_peer_refresh, next_heartbeat, next_monitor,
                                   next_cert_check});
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
    // Report our current endpoint to the server so other peers can find us
    // The endpoint comes from the tunnel's current status
    std::string endpoint;
    {
        std::lock_guard lock(mutex_);
        endpoint = our_endpoint_;
    }

    // For now, use empty endpoint if we don't have STUN info.
    // In a future phase, we'll integrate STUN probe to learn our reflexive address.
    nlohmann::json body;
    body["node_id"]  = node_id_;
    body["endpoint"] = endpoint;

    auto result = client_.mesh_heartbeat(node_id_, endpoint);
    if (!result) {
        spdlog::debug("[MeshOrchestrator] heartbeat failed: {}", result.error);
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
