#include "lemon/server/api_route.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Reports current host resource usage as measured by the server process, for "
            "first-party clients and dashboards that want lightweight telemetry without "
            "scraping Prometheus.";
        s.notes = {
            "GPU, VRAM and NPU readings depend on the operating system and installed drivers; "
            "an unavailable reading is `null`. `HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["cpu_percent", "memory_gb", "gpu_percent", "vram_gb", "npu_percent"],
            "properties": {
                "cpu_percent": {"type": ["number", "null"], "description": "System CPU utilization since the previous reading."},
                "memory_gb": {"type": "number", "description": "System RAM in use, in GiB."},
                "gpu_percent": {"type": ["number", "null"], "description": "GPU utilization."},
                "vram_gb": {"type": ["number", "null"], "description": "GPU memory in use, in GiB."},
                "npu_percent": {"type": ["number", "null"], "description": "NPU utilization."}
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
