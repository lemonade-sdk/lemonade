#include <lemon/system_metrics_platform.h>
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

namespace lemon {

namespace {

// PDH counter paths are localized by the OS, so building a path from the
// English counter names ("GPU Engine", "Utilization Percentage", ...) breaks
// on non-English Windows. Resolve the current-locale string from the canonical
// English name via its stable performance-index: the index is machine-specific
// but locale-neutral, and PdhLookupPerfIndexByName accepts the English name
// regardless of the OS language.
std::string localized_perf_name(const char* english_name) {
    DWORD index = 0;
    if (PdhLookupPerfIndexByNameA(NULL, english_name, &index) != ERROR_SUCCESS ||
        index == 0) {
        return english_name;
    }
    DWORD len = 0;
    if (PdhLookupPerfNameByIndexA(NULL, index, NULL, &len) != PDH_MORE_DATA) {
        return english_name;
    }
    std::string buf(len, '\0');
    if (PdhLookupPerfNameByIndexA(NULL, index, buf.data(), &len) != ERROR_SUCCESS) {
        return english_name;
    }
    buf.resize(std::strlen(buf.c_str()));
    return buf;
}

} // namespace

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
        std::lock_guard<std::mutex> lock(pdh_mutex_);
        if (!ensure_query()) {
            return -1.0;
        }
        collect_if_stale();
        return cached_gpu_usage_;
    }

    double get_vram_usage_gb() override {
        std::lock_guard<std::mutex> lock(pdh_mutex_);
        if (!ensure_query()) {
            return -1.0;
        }
        collect_if_stale();
        return cached_vram_gb_;
    }

    double get_npu_utilization() override {
        // NPU monitoring not implemented for Windows
        return -1.0;
    }

    ~WindowsMetricsPlatform() override {
        if (query_ != NULL) {
            PdhCloseQuery(query_);
            query_ = NULL;
        }
    }

private:
    // Build the query and counters once. Returns false if no usable counters
    // exist (e.g. no GPU present).
    bool ensure_query() {
        if (initialized_) {
            return !gpu_counters_.empty() || !vram_counters_.empty();
        }
        initialized_ = true;

        PDH_HQUERY query = NULL;
        if (PdhOpenQuery(NULL, 0, &query) != ERROR_SUCCESS) {
            return false;
        }

        // PDH instead of DXGI QueryVideoMemoryInfo: the latter reports
        // CurrentUsage=0 on AMD iGPU drivers, hiding the status bar line.
        const std::string gpu_obj = localized_perf_name("GPU Engine");
        const std::string gpu_ctr = localized_perf_name("Utilization Percentage");
        const std::string mem_obj = localized_perf_name("GPU Adapter Memory");
        const std::string mem_ctr = localized_perf_name("Dedicated Usage");

        add_counters(query, gpu_obj, gpu_ctr, true, gpu_counters_);
        add_counters(query, mem_obj, mem_ctr, false, vram_counters_);

        if (gpu_counters_.empty() && vram_counters_.empty()) {
            PdhCloseQuery(query);
            return false;
        }

        query_ = query;
        return true;
    }

    void add_counters(PDH_HQUERY query, const std::string& object,
                     const std::string& counter, bool all_instances,
                     std::vector<PDH_HCOUNTER>& out) {
        DWORD counter_buf_len = 0;
        DWORD instance_buf_len = 0;
        PDH_STATUS st = PdhEnumObjectItemsA(NULL, NULL, object.c_str(),
                                            NULL, &counter_buf_len,
                                            NULL, &instance_buf_len,
                                            PERF_DETAIL_WIZARD, 0);
        if (st != PDH_MORE_DATA || instance_buf_len == 0) {
            return;
        }

        std::vector<char> counters_buf(counter_buf_len);
        std::vector<char> instances_buf(instance_buf_len);
        st = PdhEnumObjectItemsA(NULL, NULL, object.c_str(),
                                 counters_buf.data(), &counter_buf_len,
                                 instances_buf.data(), &instance_buf_len,
                                 PERF_DETAIL_WIZARD, 0);
        if (st != ERROR_SUCCESS) {
            return;
        }

        for (char* name = instances_buf.data(); *name; name += strlen(name) + 1) {
            if (!all_instances && strncmp(name, "luid_", 5) != 0) {
                continue;
            }
            char path[512];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "\\%s(%s)\\%s",
                        object.c_str(), name, counter.c_str());
            PDH_HCOUNTER handle = NULL;
            if (PdhAddCounterA(query, path, 0, &handle) == ERROR_SUCCESS) {
                out.push_back(handle);
            }
        }
    }

    // PDH rate counters (GPU utilization) measure activity between successive
    // PdhCollectQueryData calls; the app's own polling interval supplies the
    // delta. Re-collect at most once per kCollectDebounceMs so the rate is not
    // zeroed by the back-to-back gpu/vram reads in a single metrics pass.
    void collect_if_stale() {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_collect_);
        if (elapsed.count() < kCollectDebounceMs) {
            return;
        }
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return;
        }
        last_collect_ = now;

        cached_gpu_usage_ = read_max_double(gpu_counters_);
        cached_vram_gb_ = read_sum_bytes(vram_counters_);
    }

    double read_max_double(const std::vector<PDH_HCOUNTER>& counters) {
        if (counters.empty()) {
            return -1.0;
        }
        double max_usage = -1.0;
        PDH_FMT_COUNTERVALUE value{};
        for (PDH_HCOUNTER counter : counters) {
            if (PdhGetFormattedCounterValue(
                    counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &value) ==
                    ERROR_SUCCESS &&
                value.doubleValue > max_usage) {
                max_usage = value.doubleValue;
            }
        }
        if (max_usage < 0) {
            return -1.0;
        }
        return std::round(max_usage * 10.0) / 10.0;
    }

    double read_sum_bytes(const std::vector<PDH_HCOUNTER>& counters) {
        if (counters.empty()) {
            return -1.0;
        }
        double total_bytes = 0.0;
        bool any = false;
        PDH_FMT_COUNTERVALUE value{};
        for (PDH_HCOUNTER counter : counters) {
            if (PdhGetFormattedCounterValue(
                    counter, PDH_FMT_LARGE | PDH_FMT_NOCAP100, NULL, &value) ==
                    ERROR_SUCCESS &&
                value.largeValue > 0) {
                total_bytes += static_cast<double>(value.largeValue);
                any = true;
            }
        }
        if (!any) {
            return -1.0;
        }
        double gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
        return std::round(gb * 10.0) / 10.0;
    }

    static constexpr int64_t kCollectDebounceMs = 100;

    PDH_HQUERY query_ = NULL;
    std::vector<PDH_HCOUNTER> gpu_counters_;
    std::vector<PDH_HCOUNTER> vram_counters_;
    double cached_gpu_usage_ = -1.0;
    double cached_vram_gb_ = -1.0;
    std::chrono::steady_clock::time_point last_collect_{};
    bool initialized_ = false;
    std::mutex pdh_mutex_;
};

std::unique_ptr<SystemMetricsPlatform> create_metrics_platform() {
    return std::make_unique<WindowsMetricsPlatform>();
}

} // namespace lemon
