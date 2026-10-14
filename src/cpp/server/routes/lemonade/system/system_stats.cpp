#include "lemon/server/api_route.h"
#include "lemon/server/system_stats.h"

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
            "required": ["cpu_percent", "memory_gb", "gpu_percent", "vram_gb", "primary_gpu_id", "gpus", "npu_percent"],
            "properties": {
                "cpu_percent": {"type": ["number", "null"], "description": "System CPU utilization percentage, or `null` when unavailable"},
                "memory_gb": {"type": "number", "description": "System RAM currently in use, in GiB"},
                "gpu_percent": {"type": ["number", "null"], "description": "GPU utilization percentage, or `null` when unavailable"},
                "vram_gb": {"type": ["number", "null"], "description": "GPU memory currently in use, in GiB, or `null` when unavailable"},
                "primary_gpu_id": {"type": ["string", "null"], "description": "`id` of the GPU that `gpu_percent` and `vram_gb` describe, or `null` when no GPU is enumerated"},
                "gpus": {
                    "type": "array",
                    "description": "Every enumerated host GPU, primary first",
                    "items": {
                        "type": "object",
                        "required": ["id", "vendor", "name", "memory_type", "gpu_percent", "memory_total_gb", "memory_used_gb"],
                        "properties": {
                            "id": {"type": "string", "description": "Stable device ID, e.g. `amd:0000:c1:00.0`"},
                            "vendor": {"type": "string", "description": "`amd` or `nvidia`"},
                            "name": {"type": "string", "description": "Device name"},
                            "memory_type": {"enum": ["dedicated", "shared", "unknown"], "description": "`dedicated`, `shared`, or `unknown`"},
                            "gpu_percent": {"type": ["number", "null"], "description": "GPU utilization percentage, or `null` when unavailable"},
                            "memory_total_gb": {"type": ["number", "null"], "description": "GPU-accessible memory capacity, in GiB, or `null` when unavailable"},
                            "memory_used_gb": {"type": ["number", "null"], "description": "GPU memory currently in use, in GiB, or `null` when unavailable"}
                        }
                    }
                },
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

        res.set_content(system_stats_json(*ctx_.metrics).dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_system_stats_route(ServerContext& ctx) {
    return std::make_unique<SystemStatsRoute>(ctx);
}

} // namespace lemon
