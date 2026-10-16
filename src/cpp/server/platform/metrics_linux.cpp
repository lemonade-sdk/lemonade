#include <lemon/system_metrics_platform.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <vector>
#include <utility>
#include <chrono>
#include <map>
#include <mutex>
#include "nvidia_metrics.h"
#include "amd_gpu_type.h"
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libdrm/drm.h>
#include <lemon/amdxdna_accel.h>

namespace fs = std::filesystem;

namespace lemon {

namespace {

bool read_sysfs_u64(const fs::path& path, uint64_t& result) {
    std::ifstream file(path);
    return file.is_open() && static_cast<bool>(file >> result);
}

struct CachedAmdGpuInfo {
    bool shared = false;
    bool confirmed = false;
    std::string marketing_name;
    std::chrono::steady_clock::time_point retry_after{};
};

CachedAmdGpuInfo amd_static_info(const fs::path& card_path,
                                 const std::string& id) {
    static std::mutex mutex;
    static std::map<std::string, CachedAmdGpuInfo> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto& value = cache[id];
    const auto now = std::chrono::steady_clock::now();
    // Retry inconclusive probes occasionally; never freeze a sandbox guess.
    if (!value.confirmed && now >= value.retry_after) {
        value.shared = gpu_metrics_detail::is_amd_apu(
            card_path, &value.marketing_name, true, &value.confirmed);
        value.retry_after = now + std::chrono::seconds(30);
    }
    return value;
}

std::vector<GpuDeviceMetrics> query_amd_gpu_metrics() {
    std::vector<GpuDeviceMetrics> devices;
    try {
        const fs::path drm_path = "/sys/class/drm";
        if (!fs::is_directory(drm_path)) return devices;
        for (const auto& entry : fs::directory_iterator(drm_path)) {
            const std::string card = entry.path().filename().string();
            if (card.rfind("card", 0) != 0 || card.find('-') != std::string::npos)
                continue;
            const fs::path device_path = entry.path() / "device";
            std::ifstream vendor_file(device_path / "vendor");
            std::string vendor;
            if (!std::getline(vendor_file, vendor) || vendor != "0x1002")
                continue;

            GpuDeviceMetrics gpu;
            std::error_code ec;
            const auto canonical = fs::canonical(device_path, ec);
            gpu.id = "amd:" + (ec ? "drm:" + card : canonical.filename().string());
            gpu.vendor = "amd";
            gpu.name = "AMD GPU (" + card + ")";
            const auto static_info = amd_static_info(entry.path(), gpu.id);
            const bool shared = static_info.shared;
            if (!static_info.marketing_name.empty())
                gpu.name = static_info.marketing_name;
            gpu.memory_type = shared ? "shared" : "dedicated";
            std::ifstream usage_file(device_path / "gpu_busy_percent");
            double usage = -1.0;
            if (usage_file >> usage) gpu.gpu_percent = usage;

            uint64_t vram_total = 0, vram_used = 0;
            uint64_t gtt_total = 0, gtt_used = 0;
            const bool has_vram_total = read_sysfs_u64(
                device_path / "mem_info_vram_total", vram_total);
            const bool has_vram_used = read_sysfs_u64(
                device_path / "mem_info_vram_used", vram_used);
            const bool has_gtt_total = read_sysfs_u64(
                device_path / "mem_info_gtt_total", gtt_total);
            const bool has_gtt_used = read_sysfs_u64(
                device_path / "mem_info_gtt_used", gtt_used);

            if (shared) {
                if (has_vram_total && has_gtt_total)
                    gpu.memory_total_gb = static_cast<double>(vram_total + gtt_total)
                        / BYTES_PER_GIB;
                if (has_vram_used && has_gtt_used)
                    gpu.memory_used_gb = static_cast<double>(vram_used + gtt_used)
                        / BYTES_PER_GIB;
            } else {
                if (has_vram_total)
                    gpu.memory_total_gb = static_cast<double>(vram_total) / BYTES_PER_GIB;
                if (has_vram_used)
                    gpu.memory_used_gb = static_cast<double>(vram_used) / BYTES_PER_GIB;
            }
            devices.push_back(std::move(gpu));
        }
    } catch (const fs::filesystem_error&) {
        // A card disappearing during enumeration is handled by the tracker.
    }
    return devices;
}

} // namespace

class LinuxMetricsPlatform : public SystemMetricsPlatform {
public:
    const char* get_platform_name() const override {
        return "Linux";
    }

    double get_cpu_usage(std::mutex& cpu_stats_mutex,
                        uint64_t& last_total,
                        uint64_t& last_total_idle) override {
        std::lock_guard<std::mutex> lock(cpu_stats_mutex);

        std::ifstream stat_file("/proc/stat");
        if (!stat_file.is_open()) {
            return -1.0;
        }

        std::string line;
        std::getline(stat_file, line);
        stat_file.close();

        // Parse: "cpu  user nice system idle iowait irq softirq steal"
        std::istringstream iss(line);
        std::string cpu_label;
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

        iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        uint64_t total_idle = idle + iowait;
        uint64_t total_active = user + nice + system + irq + softirq + steal;
        uint64_t total = total_idle + total_active;

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
        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo.is_open()) {
            return 0.0;
        }

        std::string line;
        long long total_kb = 0, available_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                sscanf(line.c_str(), "MemTotal: %lld kB", &total_kb);
            } else if (line.find("MemAvailable:") == 0) {
                sscanf(line.c_str(), "MemAvailable: %lld kB", &available_kb);
                break;
            }
        }
        meminfo.close();

        double used_gb = (total_kb - available_kb) / (1024.0 * 1024.0);
        return std::round(used_gb * 10.0) / 10.0;
    }

    double get_gpu_usage() override {
        return get_system_gpu_metrics().gpu_percent;
    }

    double get_vram_usage_gb() override {
        return get_system_gpu_metrics().vram_used_gb;
    }

    SystemGpuMetrics get_system_gpu_metrics() override {
        auto devices = query_amd_gpu_metrics();
        auto nvidia = query_nvidia_gpu_metrics();
        devices.insert(devices.end(), nvidia.begin(), nvidia.end());
        return gpu_tracker_.update(devices);
    }

    double get_npu_utilization() override {
        try {
            std::string accel_path = "/dev/accel/accel0";
            if (!fs::exists(accel_path)) {
                return -1.0;
            }

            int fd = open(accel_path.c_str(), O_RDWR);
            if (fd < 0) {
                return -1.0;
            }

            // Check DRM API version (must be 0.7 or later for these IOCTLs)
            struct drm_version drm_v;
            memset(&drm_v, 0, sizeof(drm_v));
            bool version_ok = false;
            if (ioctl(fd, DRM_IOCTL_VERSION, &drm_v) == 0) {
                if (drm_v.version_major > 0 || (drm_v.version_major == 0 && drm_v.version_minor >= 7)) {
                    version_ok = true;
                }
            }

            if (!version_ok) {
                close(fd);
                return -1.0;
            }

            // Check power_state to avoid waking the NPU if it is asleep
            fs::path power_state_path = "/sys/class/accel/accel0/device/power_state";
            if (fs::exists(power_state_path)) {
                std::ifstream power_file(power_state_path);
                std::string state;
                if (power_file >> state) {
                    if (state != "D0") {
                        close(fd);
                        return 0.0;
                    }
                }
            }

            // Query NPU utilization via sensor API
            amdxdna_drm_query_sensor sensors[16] = {};
            amdxdna_drm_get_info get_info = {};
            get_info.param = DRM_AMDXDNA_QUERY_SENSORS;
            get_info.buffer_size = sizeof(sensors);
            get_info.buffer = (uintptr_t)sensors;

            if (ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info) < 0) {
                close(fd);
                return -1.0;
            }

            close(fd);

            int num_sensors = get_info.buffer_size / sizeof(amdxdna_drm_query_sensor);
            double usage_sum = 0.0;
            int usage_count = 0;
            for (int i = 0; i < num_sensors; ++i) {
                if (sensors[i].type == AMDXDNA_SENSOR_TYPE_COLUMN_UTILIZATION) {
                    double val = (double)sensors[i].input * std::pow(10.0, sensors[i].unitm);
                    usage_sum += val;
                    usage_count++;
                }
            }

            if (usage_count > 0) {
                // Return average utilization percentage [0, 100]
                return (usage_sum / usage_count);
            }

            return -1.0;
        } catch (...) {
            return -1.0;
        }
    }
private:
    GpuMetricsTracker gpu_tracker_;
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<LinuxMetricsPlatform>();
}

} // namespace lemon
