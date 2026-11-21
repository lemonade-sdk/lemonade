#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace lemon {

struct GpuDeviceMetrics {
    std::string id;
    std::string vendor;
    std::string name;
    std::string memory_type = "unknown";
    double gpu_percent = -1.0;
    double memory_total_gb = -1.0;
    double memory_used_gb = -1.0;
};

struct SystemGpuMetrics {
    double gpu_percent = -1.0;
    double vram_used_gb = -1.0;
    std::string primary_gpu_id;
    std::vector<GpuDeviceMetrics> gpus;
};

// Preserve the device discovery order (NVML index order for NVIDIA), independent
// of memory capacity or activity. Remember metadata, but never stale live usage.
class GpuMetricsTracker {
public:
    SystemGpuMetrics update(
        const std::vector<GpuDeviceMetrics>& samples,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, device] : known_) {
            device.gpu_percent = -1.0;
            device.memory_used_gb = -1.0;
        }
        for (const auto& sample : samples) {
            if (sample.id.empty()) continue;
            if (known_.find(sample.id) == known_.end()) order_.push_back(sample.id);
            last_seen_[sample.id] = now;
            auto& device = known_[sample.id];
            device.id = sample.id;
            if (!sample.vendor.empty()) device.vendor = sample.vendor;
            if (!sample.name.empty()) device.name = sample.name;
            if (!sample.memory_type.empty() && sample.memory_type != "unknown")
                device.memory_type = sample.memory_type;
            if (std::isfinite(sample.memory_total_gb) && sample.memory_total_gb >= 0.0)
                device.memory_total_gb = sample.memory_total_gb;
            if (std::isfinite(sample.gpu_percent) && sample.gpu_percent >= 0.0 &&
                sample.gpu_percent <= 100.0)
                device.gpu_percent = sample.gpu_percent;
            if (std::isfinite(sample.memory_used_gb) && sample.memory_used_gb >= 0.0)
                device.memory_used_gb = sample.memory_used_gb;
        }
        // Process current samples before expiry so returning GPUs keep their slot.
        for (auto it = known_.begin(); it != known_.end();) {
            if (now - last_seen_.at(it->first) > std::chrono::minutes(5)) {
                last_seen_.erase(it->first);
                it = known_.erase(it);
            } else {
                ++it;
            }
        }
        order_.erase(std::remove_if(order_.begin(), order_.end(),
            [this](const std::string& id) { return known_.find(id) == known_.end(); }),
            order_.end());
        SystemGpuMetrics result;
        for (const auto& id : order_) result.gpus.push_back(known_.at(id));
        if (!result.gpus.empty()) {
            const auto& primary = result.gpus.front();
            result.primary_gpu_id = primary.id;
            result.gpu_percent = primary.gpu_percent;
            result.vram_used_gb = primary.memory_used_gb;
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::map<std::string, GpuDeviceMetrics> known_;
    std::vector<std::string> order_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_seen_;
};


inline constexpr double BYTES_PER_GIB = 1024.0 * 1024.0 * 1024.0;

// Platform-specific system metrics collection
class SystemMetricsPlatform {
public:
    virtual ~SystemMetricsPlatform() = default;

    // Platform name for identification
    virtual const char* get_platform_name() const = 0;

    // CPU usage percentage (0-100), -1 if not available
    // Requires mutex for delta tracking
    virtual double get_cpu_usage(std::mutex& cpu_stats_mutex,
                                 uint64_t& last_total,
                                 uint64_t& last_total_idle) = 0;

    // Memory usage in GB, 0 if not available
    virtual double get_memory_usage_gb() = 0;

    // Total physical RAM in GB, 0 if not available
    virtual double get_memory_total_gb() { return 0.0; }

    // GPU usage percentage (0-100), -1 if not available or unsupported
    virtual double get_gpu_usage() = 0;

    // VRAM usage in GB, -1 if not available or unsupported
    virtual double get_vram_usage_gb() = 0;

    virtual SystemGpuMetrics get_system_gpu_metrics() {
        SystemGpuMetrics result;
        result.gpu_percent = get_gpu_usage();
        result.vram_used_gb = get_vram_usage_gb();
        return result;
    }

    // NPU utilization percentage (0-100), -1 if not available or unsupported
    virtual double get_npu_utilization() = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<SystemMetricsPlatform> create_metrics_platform();

} // namespace lemon
