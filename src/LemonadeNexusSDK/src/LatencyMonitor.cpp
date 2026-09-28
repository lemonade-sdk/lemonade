#include <LemonadeNexusSDK/LatencyMonitor.hpp>

#include <httplib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace lnsdk {

// ---------------------------------------------------------------------------
// Internal per-server tracking (one entry per ServerEndpoint)
// ---------------------------------------------------------------------------

struct LatencyServerEntry {
    ServerEndpoint endpoint;
    double         smoothed_rtt_ms{0.0};
    bool           reachable{false};
    int64_t        last_probe_time{0};   // ms since epoch (steady_clock)
    int            consecutive_failures{0};
    bool           has_samples{false};   // true after first RTT sample
};

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct LatencyMonitor::Impl {
    LatencyConfig               config;
    std::vector<LatencyServerEntry>    entries;
    mutable std::size_t         current_idx{0};
    ServerSwitchCallback        switch_cb;

    // Cooldown tracking
    std::chrono::steady_clock::time_point last_switch_time{};
    bool                                  has_switched{false};

    // Background thread
    std::thread                 probe_thread;
    std::atomic<bool>           running{false};
    mutable std::mutex          mtx;
    std::condition_variable     cv;           // for waking up / stopping the thread

    explicit Impl(const LatencyConfig& cfg) : config{cfg} {}

    // Measure RTT to a single endpoint via HTTPS GET /api/health
    double probe_one(const ServerEndpoint& ep) const {
        // Always HTTPS (verified by FQDN); the mesh has no plaintext path.
        std::string url = "https://" + ep.host + ":" + std::to_string(ep.port);

        httplib::Client cli(url);
        cli.set_connection_timeout(0, config.probe_timeout_ms * 1000);  // microseconds
        cli.set_read_timeout(0, config.probe_timeout_ms * 1000);

        auto start = std::chrono::steady_clock::now();
        auto res = cli.Get("/api/health");
        auto end = std::chrono::steady_clock::now();

        if (!res || res->status != 200) {
            return -1.0; // unreachable
        }
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    // Probe all servers and update their stats.
    // Blocking I/O (HTTP probes) happens OUTSIDE the lock to avoid
    // starving other threads that need to read or update shared state.
    void probe_all() {
        // 1. Snapshot the endpoint list while holding the lock.
        std::vector<ServerEndpoint> targets;
        {
            std::lock_guard lock(mtx);
            targets.reserve(entries.size());
            for (const auto& e : entries) {
                targets.push_back(e.endpoint);
            }
        }

        // 2. Perform blocking HTTP probes WITHOUT holding the lock.
        struct ProbeResult {
            double  rtt_ms;   // negative means unreachable
            int64_t probe_time;
        };
        std::vector<ProbeResult> results;
        results.reserve(targets.size());

        for (const auto& ep : targets) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            double rtt = probe_one(ep);
            results.push_back({rtt, now_ms});
        }

        // 3. Re-acquire the lock to merge results into shared state.
        {
            std::lock_guard lock(mtx);

            // The entry list may have changed while we were probing (e.g. a
            // call to set_servers).  Match results by endpoint identity so we
            // don't corrupt state if the list was modified.
            for (std::size_t i = 0; i < targets.size(); ++i) {
                auto idx = find_endpoint(targets[i]);
                if (idx == static_cast<std::size_t>(-1)) {
                    continue; // entry was removed while we were probing
                }
                auto& e = entries[idx];
                double rtt = results[i].rtt_ms;
                e.last_probe_time = results[i].probe_time;

                if (rtt < 0.0) {
                    e.consecutive_failures++;
                    if (e.consecutive_failures >= 3) {
                        e.reachable = false;
                    }
                    spdlog::debug("[LatencyMonitor] probe failed for {}:{} (failures={})",
                                   e.endpoint.host, e.endpoint.port, e.consecutive_failures);
                } else {
                    e.consecutive_failures = 0;
                    e.reachable = true;
                    if (!e.has_samples) {
                        e.smoothed_rtt_ms = rtt;
                        e.has_samples = true;
                    } else {
                        e.smoothed_rtt_ms = config.ema_alpha * rtt +
                                            (1.0 - config.ema_alpha) * e.smoothed_rtt_ms;
                    }
                    spdlog::debug("[LatencyMonitor] probe {}:{} rtt={:.1f}ms smoothed={:.1f}ms",
                                   e.endpoint.host, e.endpoint.port, rtt, e.smoothed_rtt_ms);
                }
            }
        }
    }

    // Check if we should switch servers.  Returns index of better server or
    // current_idx if no switch is warranted.
    std::size_t evaluate_switch() const {
        if (entries.empty()) return current_idx;
        if (current_idx >= entries.size()) current_idx = 0;
        const auto& cur = entries[current_idx];

        // Only consider switching if current RTT exceeds threshold
        if (!cur.has_samples || cur.smoothed_rtt_ms <= config.threshold_ms) {
            return current_idx;
        }

        double best_rtt = cur.smoothed_rtt_ms;
        std::size_t best_idx = current_idx;

        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i == current_idx) continue;
            const auto& e = entries[i];
            if (!e.reachable || !e.has_samples) continue;

            // Must be at least hysteresis% better
            if (e.smoothed_rtt_ms < cur.smoothed_rtt_ms * (1.0 - config.hysteresis)) {
                if (e.smoothed_rtt_ms < best_rtt) {
                    best_rtt = e.smoothed_rtt_ms;
                    best_idx = i;
                }
            }
        }
        return best_idx;
    }

    // Background loop
    void run() {
        while (running.load(std::memory_order_relaxed)) {
            probe_all();

            // Evaluate whether we should switch servers.
            ServerSwitchCallback cb_copy;
            ServerConfig new_server;
            bool do_switch = false;

            {
                std::lock_guard lock(mtx);
                auto candidate = evaluate_switch();
                if (candidate != current_idx) {
                    // Cooldown check
                    auto now = std::chrono::steady_clock::now();
                    bool cooldown_ok = !has_switched ||
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_switch_time).count() >=
                            static_cast<int64_t>(config.cooldown_sec);

                    if (cooldown_ok) {
                        spdlog::info("[LatencyMonitor] switching from {}:{} ({:.1f}ms) "
                                     "to {}:{} ({:.1f}ms)",
                                     entries[current_idx].endpoint.host,
                                     entries[current_idx].endpoint.port,
                                     entries[current_idx].smoothed_rtt_ms,
                                     entries[candidate].endpoint.host,
                                     entries[candidate].endpoint.port,
                                     entries[candidate].smoothed_rtt_ms);

                        current_idx = candidate;
                        last_switch_time = now;
                        has_switched = true;

                        // Build a ServerConfig for just the new endpoint
                        new_server.servers = {entries[candidate].endpoint};
                        cb_copy = switch_cb;
                        do_switch = true;
                    }
                }
            }

            // Fire callback outside the lock to avoid deadlock
            if (do_switch && cb_copy) {
                try {
                    cb_copy(new_server);
                } catch (const std::exception& ex) {
                    spdlog::error("[LatencyMonitor] switch callback threw: {}", ex.what());
                }
            }

            // Sleep until next probe interval (or until stop is called)
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::seconds(config.probe_interval_sec),
                            [this] { return !running.load(std::memory_order_relaxed); });
            }
        }
    }

    // Find the index of a ServerEndpoint by host:port match, or npos
    std::size_t find_endpoint(const ServerEndpoint& ep) const {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].endpoint.host == ep.host &&
                entries[i].endpoint.port == ep.port) {
                return i;
            }
        }
        return static_cast<std::size_t>(-1);
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

LatencyMonitor::LatencyMonitor(const LatencyConfig& config)
    : impl_{std::make_unique<Impl>(config)}
{
}

LatencyMonitor::~LatencyMonitor() {
    stop();
}

void LatencyMonitor::set_servers(const std::vector<ServerConfig>& servers) {
    std::lock_guard lock(impl_->mtx);
    impl_->entries.clear();
    for (const auto& cfg : servers) {
        for (const auto& ep : cfg.servers) {
            // Avoid duplicates
            bool found = false;
            for (const auto& existing : impl_->entries) {
                if (existing.endpoint.host == ep.host &&
                    existing.endpoint.port == ep.port) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LatencyServerEntry entry;
                entry.endpoint = ep;
                impl_->entries.push_back(std::move(entry));
            }
        }
    }
    if (!impl_->entries.empty()) {
        impl_->current_idx = 0;
    }
}

void LatencyMonitor::set_current_server(const ServerConfig& server) {
    std::lock_guard lock(impl_->mtx);
    if (!server.servers.empty()) {
        auto idx = impl_->find_endpoint(server.servers[0]);
        if (idx != static_cast<std::size_t>(-1)) {
            impl_->current_idx = idx;
        }
    }
}

void LatencyMonitor::record_rtt(double rtt_ms) {
    std::lock_guard lock(impl_->mtx);
    if (impl_->entries.empty()) return;
    if (impl_->current_idx >= impl_->entries.size()) impl_->current_idx = 0;
    auto& cur = impl_->entries[impl_->current_idx];

    if (!cur.has_samples) {
        cur.smoothed_rtt_ms = rtt_ms;
        cur.has_samples = true;
    } else {
        cur.smoothed_rtt_ms = impl_->config.ema_alpha * rtt_ms +
                              (1.0 - impl_->config.ema_alpha) * cur.smoothed_rtt_ms;
    }
    cur.reachable = true;
    cur.consecutive_failures = 0;
}

void LatencyMonitor::set_switch_callback(ServerSwitchCallback cb) {
    std::lock_guard lock(impl_->mtx);
    impl_->switch_cb = std::move(cb);
}

void LatencyMonitor::start() {
    if (impl_->running.exchange(true)) return; // already running
    impl_->probe_thread = std::thread([this] { impl_->run(); });
}

void LatencyMonitor::stop() {
    if (!impl_->running.exchange(false)) return; // already stopped
    {
        std::lock_guard lock(impl_->mtx);
        impl_->cv.notify_all();
    }
    if (impl_->probe_thread.joinable()) {
        impl_->probe_thread.join();
    }
}

std::vector<ServerLatency> LatencyMonitor::get_stats() const {
    std::lock_guard lock(impl_->mtx);
    std::vector<ServerLatency> out;
    out.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) {
        ServerLatency sl;
        sl.server.servers = {e.endpoint};
        sl.smoothed_rtt_ms       = e.smoothed_rtt_ms;
        sl.reachable             = e.reachable;
        sl.last_probe_time       = e.last_probe_time;
        sl.consecutive_failures  = e.consecutive_failures;
        out.push_back(std::move(sl));
    }
    return out;
}

double LatencyMonitor::current_rtt_ms() const {
    std::lock_guard lock(impl_->mtx);
    if (impl_->entries.empty()) return 0.0;
    if (impl_->current_idx >= impl_->entries.size()) impl_->current_idx = 0;
    return impl_->entries[impl_->current_idx].smoothed_rtt_ms;
}

void LatencyMonitor::probe_now() {
    impl_->probe_all();
}

} // namespace lnsdk
