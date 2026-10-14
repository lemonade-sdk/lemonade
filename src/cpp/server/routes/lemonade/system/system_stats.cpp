#include "lemon/server/api_route.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class SystemStatsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.system_stats";
        s.methods = {"GET"};
        s.paths = {"system-stats"};
        s.summary = "Current host resource usage";
        s.description =
            "Current host resource usage as measured by the Lemonade Server process. This endpoint "
            "is useful for first-party clients and dashboards that need lightweight runtime "
            "telemetry without scraping Prometheus.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["cpu_percent", "memory_gb", "gpu_percent", "vram_gb", "npu_percent"],
            "properties": {
                "cpu_percent": {"type": ["number", "null"], "description": "System CPU utilization percentage, or `null` when unavailable"},
                "memory_gb": {"type": "number", "description": "System RAM currently in use, in GiB"},
                "gpu_percent": {"type": ["number", "null"], "description": "GPU utilization percentage, or `null` when unavailable"},
                "vram_gb": {"type": ["number", "null"], "description": "GPU memory currently in use, in GiB, or `null` when unavailable"},
                "npu_percent": {"type": ["number", "null"], "description": "NPU utilization percentage, or `null` when unavailable"}
            }
        })");
        response.example = json::object();
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        SystemMetricsPlatform& metrics = *ctx_.metrics;
        json stats;
        double cpu_percent = metrics.get_cpu_usage();
        stats["cpu_percent"] = (cpu_percent >= 0) ? json(cpu_percent) : json();
        stats["memory_gb"] = metrics.get_memory_usage_gb();
        double gpu_percent = metrics.get_gpu_usage();
        stats["gpu_percent"] = (gpu_percent >= 0) ? json(gpu_percent) : json();
        double vram_gb = metrics.get_vram_usage_gb();
        stats["vram_gb"] = (vram_gb >= 0) ? json(vram_gb) : json();
        double npu_percent = metrics.get_npu_utilization();
        stats["npu_percent"] = (npu_percent >= 0) ? json(npu_percent) : json();

        res.set_content(stats.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_system_stats_route(ServerContext& ctx) {
    return std::make_unique<SystemStatsRoute>(ctx);
}

} // namespace lemon
