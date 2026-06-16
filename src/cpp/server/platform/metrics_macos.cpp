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
        // macOS: Could use host_processor_info or top command
        // Not implemented yet
        return -1.0;
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
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<MacOSMetricsPlatform>();
}

} // namespace lemon
