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
        s.summary = "Read the host's resource usage";
        s.description =
            "Reports the host's resource usage as the server process measures it, for clients and "
            "dashboards that want lightweight telemetry without scraping Prometheus.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
            "`gpu_percent` and `vram_gb` come from the primary GPU: the one with the largest known "
            "GPU-accessible memory capacity (ties broken by ID), whatever its current utilization, "
            "and not necessarily the GPU a model recipe targets. `vram_gb` keeps its historical "
            "name but reports GPU memory usage, which can be shared system memory on AMD APUs.",
            "GPU, VRAM and NPU telemetry depends on the operating system and installed drivers. On "
            "Linux, AMD and NVIDIA GPUs are enumerated; on Windows, NVIDIA GPUs through NVML and AMD "
            "GPUs through DXGI, DXCore and PDH; on macOS `gpus` is empty. NVML does not report "
            "whether memory is dedicated or shared, so NVIDIA GPUs report `memory_type` as `unknown`.",
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
                            "id": {"type": "string", "description": "Device ID, stable across ordinary telemetry failures, such as `amd:0000:c1:00.0`."},
                            "vendor": {"type": "string", "description": "`amd` or `nvidia`"},
                            "name": {"type": "string", "description": "Device name"},
                            "memory_type": {"enum": ["dedicated", "shared", "unknown"], "description": "`dedicated` for the GPU's own memory, `shared` for system memory, or `unknown`."},
                            "gpu_percent": {"type": ["number", "null"], "description": "GPU utilization percentage, or `null` when unavailable"},
                            "memory_total_gb": {"type": ["number", "null"], "description": "GPU-accessible memory capacity, in GiB, or `null` when unavailable"},
                            "memory_used_gb": {"type": ["number", "null"], "description": "GPU memory currently in use, in GiB, or `null` when unavailable"}
                        }
                    }
                },
                "npu_percent": {"type": ["number", "null"], "description": "NPU utilization percentage, or `null` when unavailable"}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        res.set_content(system_stats_json(*ctx_.server, *ctx_.metrics).dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_system_stats_route(ServerContext& ctx) {
    return std::make_unique<SystemStatsRoute>(ctx);
}

} // namespace lemon
