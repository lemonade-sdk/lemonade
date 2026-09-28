#pragma once

#include <LemonadeNexusSDK/Types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lnsdk {

// Forward declarations — avoid including full headers
class LemonadeNexusClient;
class BoringtunMesh;

/// Background orchestrator for P2P mesh networking over the boringtun dataplane.
///
/// Periodically:
///   1. Fetches authorized peers from the server (GET /api/mesh/peers/{node_id})
///   2. Syncs the mesh dataplane's peer set (add/remove/update)
///   3. Reports our public endpoint via heartbeat (POST /api/mesh/heartbeat)
///   4. Monitors peer connectivity and fires state callbacks
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

    /// Force an immediate peer refresh cycle.
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

    /// Poll mesh dataplane status and fire callback if changed.
    void do_monitor();

    LemonadeNexusClient&     client_;
    BoringtunMesh&           mesh_;
    std::string              node_id_;

    MeshConfig               config_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;

    mutable std::mutex       mutex_;
    std::condition_variable  cv_;          // for waking the loop early
    std::vector<MeshPeer>    known_peers_;
    MeshTunnelStatus         last_status_;
    StateCallback            callback_;
    std::string              our_endpoint_; // last reported public endpoint
};

} // namespace lnsdk
