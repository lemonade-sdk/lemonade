#include "lemon/server/system_stats.h"

#include "lemon/server.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {

namespace {

using json = nlohmann::json;

json reading(double value) {
    return value >= 0.0 ? json(value) : json();
}

} // namespace

json system_stats_json(Server& server, SystemMetricsPlatform& metrics) {
    json stats;
    stats["cpu_percent"] = reading(server.get_cpu_usage());
    stats["memory_gb"] = metrics.get_memory_usage_gb();

    const SystemGpuMetrics gpu_metrics = metrics.get_system_gpu_metrics();
    stats["gpu_percent"] = reading(gpu_metrics.gpu_percent);
    stats["vram_gb"] = reading(gpu_metrics.vram_used_gb);
    stats["primary_gpu_id"] =
        gpu_metrics.primary_gpu_id.empty() ? json() : json(gpu_metrics.primary_gpu_id);
    stats["gpus"] = json::array();
    for (const auto& gpu : gpu_metrics.gpus) {
        stats["gpus"].push_back({
            {"id", gpu.id},
            {"vendor", gpu.vendor},
            {"name", gpu.name},
            {"memory_type", gpu.memory_type},
            {"gpu_percent", reading(gpu.gpu_percent)},
            {"memory_total_gb", reading(gpu.memory_total_gb)},
            {"memory_used_gb", reading(gpu.memory_used_gb)},
        });
    }

    stats["npu_percent"] = reading(metrics.get_npu_utilization());
    return stats;
}

} // namespace lemon
