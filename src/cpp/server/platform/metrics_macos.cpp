#include <lemon/system_metrics_platform.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <cmath>

namespace lemon {

class MacOSMetricsPlatform : public SystemMetricsPlatform {
public:
    const char* get_platform_name() const override {
        return "Darwin";
    }

    double get_cpu_usage(std::mutex& cpu_stats_mutex,
                        uint64_t& last_total,
                        uint64_t& last_total_idle) override {
        // macOS: Could use host_processor_info or top command
        // Not implemented yet
        return -1.0;
    }

    double get_memory_usage_gb() override {
        int64_t physical_memory = 0;
        size_t length = sizeof(physical_memory);
        if (sysctlbyname("hw.memsize", &physical_memory, &length, nullptr, 0) == 0) {
            // For now, just report total memory since getting free memory is complex on macOS
            double total_gb = physical_memory / (1024.0 * 1024.0 * 1024.0);
            return std::round(total_gb * 10.0) / 10.0;
        }
        return 0.0;
    }

    double get_gpu_usage() override {
        // GPU usage monitoring not implemented for macOS
        return -1.0;
    }

    double get_vram_usage_gb() override {
        // VRAM monitoring not implemented for macOS
        return -1.0;
    }

    double get_npu_utilization() override {
        // NPU monitoring not implemented for macOS
        return -1.0;
    }
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<MacOSMetricsPlatform>();
}

} // namespace lemon
