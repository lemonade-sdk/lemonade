#pragma once

#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <string>

namespace lemon {

// Callback function type: receives current VRAM usage percentage (0.0 to 1.0)
using VramPressureCallback = std::function<void(double)>;

class GlobalVramMonitor {
public:
    GlobalVramMonitor();
    ~GlobalVramMonitor();

    // Start the monitor thread
    void start(int poll_interval_ms = 2000);

    // Stop the monitor thread
    void stop();

    // Register a callback to be notified of the current VRAM usage
    void set_pressure_callback(VramPressureCallback callback);

    // Test/admin hook: synchronously fire the pressure callback with a
    // simulated usage fraction, bypassing the hardware poll.
    void simulate_pressure(double pct) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (pressure_callback_) {
            pressure_callback_(pct);
        }
    }

private:
    void monitor_loop();
    double poll_vram_usage() const;

    std::atomic<bool> running_;
    int poll_interval_ms_;
    std::thread monitor_thread_;

    std::mutex callback_mutex_;
    VramPressureCallback pressure_callback_;
};

} // namespace lemon
