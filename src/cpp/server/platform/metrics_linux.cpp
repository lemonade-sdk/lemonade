#include "nvidia_metrics.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <lemon/amdxdna_accel.h>
#include <lemon/system_metrics_platform.h>
#include <libdrm/drm.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {

namespace {

struct DrmGpuSample {
    double gpu_percent = -1.0;
    double vram_used_gb = -1.0;
    bool has_memory_telemetry = false;
};

std::vector<DrmGpuSample> query_drm_gpu_samples() {
    std::vector<DrmGpuSample> samples;

    try {
        const fs::path drm_path = "/sys/class/drm";
        if (!fs::exists(drm_path)) {
            return samples;
        }

        for (const auto& entry : fs::directory_iterator(drm_path)) {
            const std::string card_name = entry.path().filename().string();
            if (card_name.find("card") != 0 ||
                card_name.find("-") != std::string::npos) {
                continue;
            }

            const fs::path device_path = entry.path() / "device";
            DrmGpuSample sample;

            std::ifstream busy_file(device_path / "gpu_busy_percent");
            if (busy_file.is_open()) {
                double gpu_usage = -1.0;
                if (busy_file >> gpu_usage) {
                    sample.gpu_percent = gpu_usage;
                }
            }

            const bool is_dgpu = fs::exists(device_path / "board_info");

            uint64_t vram_used = 0;
            bool have_vram = false;
            std::ifstream vram_file(device_path / "mem_info_vram_used");
            if (vram_file.is_open() && (vram_file >> vram_used)) {
                have_vram = true;
            }

            uint64_t gtt_used = 0;
            bool have_gtt = false;
            std::ifstream gtt_file(device_path / "mem_info_gtt_used");
            if (gtt_file.is_open() && (gtt_file >> gtt_used)) {
                have_gtt = true;
            }

            sample.has_memory_telemetry =
                is_dgpu ? have_vram : (have_vram || have_gtt);
            if (sample.has_memory_telemetry) {
                const uint64_t card_memory =
                    is_dgpu ? vram_used : (vram_used + gtt_used);
                sample.vram_used_gb =
                    static_cast<double>(card_memory) / BYTES_PER_GIB;
            }

            if (sample.gpu_percent >= 0.0 || sample.has_memory_telemetry) {
                samples.push_back(sample);
            }
        }
    } catch (...) {
        return {};
    }

    return samples;
}

SystemGpuMetrics aggregate_drm_system_gpu_metrics(
    const std::vector<DrmGpuSample>& samples) {
    SystemGpuMetrics result;
    for (const auto& sample : samples) {
        system_metrics_detail::merge_max(
            result, {sample.gpu_percent, sample.vram_used_gb});
    }
    return result;
}

double select_legacy_drm_vram_usage(
    const std::vector<DrmGpuSample>& samples, bool& has_memory_telemetry) {
    has_memory_telemetry = false;
    double highest_usage = -1.0;
    double selected_vram_gb = -1.0;

    for (const auto& sample : samples) {
        if (!sample.has_memory_telemetry) {
            continue;
        }

        has_memory_telemetry = true;
        const double selection_usage =
            sample.gpu_percent >= 0.0 ? sample.gpu_percent : 0.0;
        if (selected_vram_gb < 0.0 || selection_usage > highest_usage) {
            highest_usage = selection_usage;
            selected_vram_gb = sample.vram_used_gb;
        }
    }

    return selected_vram_gb;
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
        const auto drm_metrics =
            aggregate_drm_system_gpu_metrics(query_drm_gpu_samples());
        if (drm_metrics.gpu_percent >= 0.0) {
            return drm_metrics.gpu_percent;
        }
        return query_primary_nvidia_metrics().gpu_percent;
    }

    double get_vram_usage_gb() override {
        const auto samples = query_drm_gpu_samples();
        bool has_memory_telemetry = false;
        const double drm_vram_gb =
            select_legacy_drm_vram_usage(samples, has_memory_telemetry);
        if (has_memory_telemetry) {
            return drm_vram_gb;
        }
        return query_primary_nvidia_metrics().vram_used_gb;
    }

    SystemGpuMetrics get_system_gpu_metrics() override {
        SystemGpuMetrics result =
            aggregate_drm_system_gpu_metrics(query_drm_gpu_samples());
        system_metrics_detail::merge_max(result, query_nvidia_metrics());
        return result;
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
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<LinuxMetricsPlatform>();
}

} // namespace lemon
