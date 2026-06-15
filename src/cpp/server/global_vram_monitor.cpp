#include "lemon/global_vram_monitor.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include <chrono>
#include <thread>

namespace lemon {

GlobalVramMonitor::GlobalVramMonitor() : running_(false), poll_interval_ms_(2000) {}

GlobalVramMonitor::~GlobalVramMonitor() {
    stop();
}

void GlobalVramMonitor::start(int poll_interval_ms) {
    if (running_) return;
    poll_interval_ms_ = poll_interval_ms;
    running_ = true;
    monitor_thread_ = std::thread(&GlobalVramMonitor::monitor_loop, this);
}

void GlobalVramMonitor::stop() {
    running_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void GlobalVramMonitor::set_pressure_callback(VramPressureCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    pressure_callback_ = callback;
}

double GlobalVramMonitor::poll_vram_usage() const {
    // Delegate to the shared cross-vendor detection in SystemInfo so we don't
    // duplicate (or drift from) the platform VRAM logic.
    return SystemInfo::get_global_vram_usage_pct();
}

void GlobalVramMonitor::monitor_loop() {
    while (running_) {
        // Skip the (potentially expensive) VRAM query entirely unless the user
        // has globally opted in. Per-model auto_evict still gets time-based
        // handling from the EvictionEngine loop; VRAM-pressure eviction is a
        // global concern and requires the global opt-in.
        RuntimeConfig* cfg = RuntimeConfig::global();
        if (cfg && cfg->auto_evict()) {
            double pct = poll_vram_usage();

            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (pressure_callback_ && pct >= 0.0) {
                pressure_callback_(pct);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms_));
    }
}

} // namespace lemon
