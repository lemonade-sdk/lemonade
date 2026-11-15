#pragma once

#include <LemonadeNexusSDK/Types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lnsdk {

struct LatencyConfig {
    double   ema_alpha{0.3};           // EMA smoothing factor
    double   threshold_ms{200.0};      // Switch threshold in ms
    double   hysteresis{0.3};          // 30% — new server must be this much better
    uint32_t cooldown_sec{60};         // Minimum seconds between switches
    uint32_t probe_interval_sec{10};   // How often to probe servers
    uint32_t probe_timeout_ms{5000};   // Probe timeout
};

struct ServerLatency {
    ServerConfig server;
    double       smoothed_rtt_ms{0.0};
    bool         reachable{false};
    int64_t      last_probe_time{0};
    int          consecutive_failures{0};
};

using ServerSwitchCallback = std::function<void(const ServerConfig& new_server)>;

class LatencyMonitor {
public:
    explicit LatencyMonitor(const LatencyConfig& config = {});
    ~LatencyMonitor();

    // Non-copyable
    LatencyMonitor(const LatencyMonitor&) = delete;
    LatencyMonitor& operator=(const LatencyMonitor&) = delete;

    /// Set the list of known servers to monitor.
    void set_servers(const std::vector<ServerConfig>& servers);

    /// Set the currently active server.
    void set_current_server(const ServerConfig& server);

    /// Record an RTT measurement for the current server (called after each API request).
    void record_rtt(double rtt_ms);

    /// Set callback for when a server switch is recommended.
    void set_switch_callback(ServerSwitchCallback cb);

    /// Start background monitoring thread.
    void start();

    /// Stop monitoring.
    void stop();

    /// Get current latency stats for all servers.
    std::vector<ServerLatency> get_stats() const;

    /// Get the smoothed RTT for the current server.
    double current_rtt_ms() const;

    /// Force a probe of all servers now.
    void probe_now();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
