#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace lemon {

// Platform-specific system metrics collection
class SystemMetricsPlatform {
public:
    virtual ~SystemMetricsPlatform() = default;

    // Platform name for identification
    virtual const char* get_platform_name() const = 0;

    // CPU usage percentage (0-100), -1 if not available. Usage is the delta since
    // the previous call, so the first call returns 0.
    virtual double get_cpu_usage() = 0;

    // Memory usage in GB, 0 if not available
    virtual double get_memory_usage_gb() = 0;

    // GPU usage percentage (0-100), -1 if not available or unsupported
    virtual double get_gpu_usage() = 0;

    // VRAM usage in GB, -1 if not available or unsupported
    virtual double get_vram_usage_gb() = 0;

    // NPU utilization percentage (0-100), -1 if not available or unsupported
    virtual double get_npu_utilization() = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<SystemMetricsPlatform> create_metrics_platform();

} // namespace lemon
