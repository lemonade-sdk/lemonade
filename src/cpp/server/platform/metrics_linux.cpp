#include <lemon/system_metrics_platform.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <vector>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libdrm/drm.h>
#include <lemon/amdxdna_accel.h>

namespace fs = std::filesystem;

namespace lemon {

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
        try {
            std::string drm_path = "/sys/class/drm";

            if (!fs::exists(drm_path)) {
                return -1.0;
            }

            double highest_usage = -1.0;

            for (const auto& entry : fs::directory_iterator(drm_path)) {
                std::string card_name = entry.path().filename().string();
                if (card_name.find("card") != 0 || card_name.find("-") != std::string::npos) {
                    continue;
                }

                std::string busy_path = entry.path().string() + "/device/gpu_busy_percent";
                std::ifstream busy_file(busy_path);
                if (busy_file.is_open()) {
                    double usage;
                    busy_file >> usage;
                    busy_file.close();
                    if (usage > highest_usage) {
                        highest_usage = usage;
                    }
                }
            }

            return highest_usage;
        } catch (...) {
            return -1.0;
        }
    }

    double get_vram_usage_gb() override {
        try {
            std::string drm_path = "/sys/class/drm";

            if (!fs::exists(drm_path)) {
                return -1.0;
            }

            double highest_usage = -1.0;
            std::string highest_card;
            double highest_card_memory = 0.0;

            for (const auto& entry : fs::directory_iterator(drm_path)) {
                std::string card_name = entry.path().filename().string();
                if (card_name.find("card") != 0 || card_name.find("-") != std::string::npos) {
                    continue;
                }

                std::string device_path = entry.path().string() + "/device";

                // Read GPU utilization to find the most active GPU
                double gpu_usage = 0.0;
                std::ifstream busy_file(device_path + "/gpu_busy_percent");
                if (busy_file.is_open()) {
                    busy_file >> gpu_usage;
                    busy_file.close();
                }

                // Check if this is a dGPU (has board_info) or APU (no board_info)
                bool is_dgpu = fs::exists(device_path + "/board_info");

                // Read VRAM used
                uint64_t vram_used = 0;
                std::ifstream vram_file(device_path + "/mem_info_vram_used");
                if (vram_file.is_open()) {
                    vram_file >> vram_used;
                    vram_file.close();
                }

                // Read GTT used
                uint64_t gtt_used = 0;
                std::ifstream gtt_file(device_path + "/mem_info_gtt_used");
                if (gtt_file.is_open()) {
                    gtt_file >> gtt_used;
                    gtt_file.close();
                }

                // Skip if no memory info found
                if (vram_used == 0 && gtt_used == 0) {
                    continue;
                }

                // Calculate memory for this card
                uint64_t card_memory = is_dgpu ? vram_used : (vram_used + gtt_used);

                // Track the GPU with highest utilization
                if (gpu_usage > highest_usage || highest_usage < 0) {
                    highest_usage = gpu_usage;
                    highest_card = card_name;
                    highest_card_memory = card_memory / (1024.0 * 1024.0 * 1024.0); // Convert to GB
                }
            }

            return highest_card_memory > 0 ? highest_card_memory : -1.0;
        } catch (...) {
            return -1.0;
        }
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
