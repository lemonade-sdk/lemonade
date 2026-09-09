#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace lemon {

inline constexpr double BYTES_PER_GIB =
    1024.0 * 1024.0 * 1024.0;

struct SystemGpuMetrics {
    double gpu_percent = -1.0;
    double vram_used_gb = -1.0;
};

namespace system_metrics_detail {

inline void merge_max(SystemGpuMetrics& result,
                      const SystemGpuMetrics& sample) {
    if (sample.gpu_percent >= 0.0 &&
        (result.gpu_percent < 0.0 ||
         sample.gpu_percent > result.gpu_percent)) {
        result.gpu_percent = sample.gpu_percent;
    }
    if (sample.vram_used_gb >= 0.0 &&
        (result.vram_used_gb < 0.0 ||
         sample.vram_used_gb > result.vram_used_gb)) {
        result.vram_used_gb = sample.vram_used_gb;
    }
}

} // namespace system_metrics_detail

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

    // GPU usage percentage (0-100), -1 if not available or unsupported
    virtual double get_gpu_usage() = 0;

    // VRAM usage in GB, -1 if not available or unsupported
    virtual double get_vram_usage_gb() = 0;

    // Multi-device monitoring is separate from the legacy scalar accessors so
    // their single-device semantics remain stable.
    virtual SystemGpuMetrics get_system_gpu_metrics() {
        return {get_gpu_usage(), get_vram_usage_gb()};
    }

    // NPU utilization percentage (0-100), -1 if not available or unsupported
    virtual double get_npu_utilization() = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<SystemMetricsPlatform> create_metrics_platform();

} // namespace lemon
