#include <lemon/system_metrics_platform.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
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
        mach_port_t host = mach_host_self();
        host_cpu_load_info_data_t load;
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        const kern_return_t status = host_statistics(host, HOST_CPU_LOAD_INFO,
                                                     reinterpret_cast<host_info_t>(&load),
                                                     &count);
        mach_port_deallocate(mach_task_self(), host);
        if (status != KERN_SUCCESS) {
            return -1.0;
        }

        // Mach tick counters are 32-bit and wrap within weeks on many-core
        // machines. Summing and subtracting in uint32_t keeps deltas correct
        // across a wrap.
        const uint32_t idle = load.cpu_ticks[CPU_STATE_IDLE];
        const uint32_t total = load.cpu_ticks[CPU_STATE_USER] +
                               load.cpu_ticks[CPU_STATE_SYSTEM] +
                               load.cpu_ticks[CPU_STATE_NICE] + idle;

        std::lock_guard<std::mutex> lock(cpu_stats_mutex);
        const bool first_sample = last_total == 0;
        const uint32_t total_diff = total - static_cast<uint32_t>(last_total);
        const uint32_t idle_diff = idle - static_cast<uint32_t>(last_total_idle);

        // Back-to-back polls can land inside one tick, and the kernel rate-limits
        // host_statistics by returning cached counters. Neither means an idle CPU.
        if (total_diff == 0) {
            return last_cpu_percent_;
        }

        last_total = total;
        last_total_idle = idle;
        if (!first_sample) {
            last_cpu_percent_ = ((total_diff - idle_diff) * 100.0) / total_diff;
        }
        return last_cpu_percent_;
    }

    double get_memory_usage_gb() override {
        // Report RAM currently in use (not total). On macOS this approximates
        // Activity Monitor's "Memory Used": (active + wired + compressed) pages.
        // Inactive/free/purgeable pages are reclaimable and counted as available.
        mach_port_t host = mach_host_self();

        vm_size_t page_size = 0;
        if (host_page_size(host, &page_size) != KERN_SUCCESS || page_size == 0) {
            mach_port_deallocate(mach_task_self(), host);
            return 0.0;
        }

        vm_statistics64_data_t vm_stats;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&vm_stats),
                              &count) != KERN_SUCCESS) {
            mach_port_deallocate(mach_task_self(), host);
            return 0.0;
        }

        mach_port_deallocate(mach_task_self(), host);

        uint64_t used_pages = static_cast<uint64_t>(vm_stats.active_count) +
                              static_cast<uint64_t>(vm_stats.wire_count) +
                              static_cast<uint64_t>(vm_stats.compressor_page_count);
        double used_gb = (used_pages * static_cast<double>(page_size)) /
                         (1024.0 * 1024.0 * 1024.0);
        return std::round(used_gb * 10.0) / 10.0;
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

private:
    double last_cpu_percent_ = 0.0;
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<MacOSMetricsPlatform>();
}

} // namespace lemon
