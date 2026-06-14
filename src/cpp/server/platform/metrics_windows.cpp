#include <lemon/system_metrics_platform.h>
#include <windows.h>
#include <cmath>

namespace lemon {

class WindowsMetricsPlatform : public SystemMetricsPlatform {
public:
    const char* get_platform_name() const override {
        return "Windows";
    }

    double get_cpu_usage(std::mutex& cpu_stats_mutex,
                        uint64_t& last_total,
                        uint64_t& last_total_idle) override {
        std::lock_guard<std::mutex> lock(cpu_stats_mutex);

        FILETIME idle_time, kernel_time, user_time;
        if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
            return -1.0;
        }

        // Convert FILETIME to uint64_t (100-nanosecond intervals)
        auto filetime_to_uint64 = [](const FILETIME& ft) -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };

        uint64_t idle = filetime_to_uint64(idle_time);
        uint64_t kernel = filetime_to_uint64(kernel_time); // Includes idle time
        uint64_t user = filetime_to_uint64(user_time);

        // Kernel time includes idle time, so subtract it to get actual kernel time
        uint64_t total = kernel + user;
        uint64_t total_idle = idle;

        if (last_total > 0) {
            uint64_t idle_diff = total_idle - last_total_idle;
            uint64_t total_diff = total - last_total;

            last_total_idle = total_idle;
            last_total = total;

            if (total_diff > 0) {
                return ((total_diff - idle_diff) * 100.0) / total_diff;
            }
        }

        last_total_idle = total_idle;
        last_total = total;
        return 0.0; // First call, no delta yet
    }

    double get_memory_usage_gb() override {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            double used_gb = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
            return std::round(used_gb * 10.0) / 10.0;
        }
        return 0.0;
    }

    double get_gpu_usage() override {
        // GPU usage monitoring not implemented for Windows
        return -1.0;
    }

    double get_vram_usage_gb() override {
        // VRAM monitoring not implemented for Windows
        return -1.0;
    }

    double get_npu_utilization() override {
        // NPU monitoring not implemented for Windows
        return -1.0;
    }
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<WindowsMetricsPlatform>();
}

} // namespace lemon
