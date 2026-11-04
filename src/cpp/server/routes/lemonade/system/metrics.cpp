#include <lemon/utils/aixlog.hpp>

#include "lemon/prometheus_metrics.h"
#include "lemon/router.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {
namespace {

using json = nlohmann::json;


class MetricsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.metrics";
        s.methods = {"GET"};
        s.paths = {"/metrics"};
        s.prefixes = Prefixes::Root;
        s.summary = "Serve metrics for Prometheus to scrape";
        s.description =
            "Serves Lemonade's metrics in the Prometheus text exposition format, for Prometheus to "
            "scrape.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
            "### Polling and Refresh Rate\n\n"
            "`/metrics` has no refresh timer of its own: each scrape renders the server's state at "
            "that moment. Prometheus's `scrape_interval` sets how often that happens, for "
            "example:\n\n"
            "```yaml\n"
            "global:\n"
            "  scrape_interval: 10s\n"
            "```\n\n"
            "Grafana reads from Prometheus, not from this endpoint, so a dashboard's refresh rate "
            "sets how often it queries Prometheus, not how often Prometheus scrapes Lemonade.",
            "### Metric Families\n"
            "\n"
            "The names, types, labels and help text are defined by the `metrics.describe(...)` "
            "calls in "
            "[`src/cpp/server/prometheus_metrics.cpp`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/server/prometheus_metrics.cpp). "
            "Unsupported, unavailable, null, NaN and infinite values are omitted rather than "
            "emitted as samples.",
            "### llama.cpp Backend Metrics\n"
            "\n"
            "Lemonade starts llama.cpp backends with metrics enabled and, for each loaded "
            "`llamacpp` model, makes a best-effort scrape of the backend's private `/metrics` "
            "endpoint. Those metrics are renamed under the `lemonade_llamacpp_*` prefix and labeled "
            "with the same model metadata as `lemonade_model_info`. A failed backend scrape does "
            "not fail the response.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Text;
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Text;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kUnexpectedException};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        try {
            SystemMetrics system_metrics;
            system_metrics.cpu_percent = ctx_.server->get_cpu_usage();
            const SystemGpuMetrics gpu_metrics = ctx_.metrics->get_system_gpu_metrics();
            system_metrics.gpu_percent = gpu_metrics.gpu_percent;
            system_metrics.vram_gb = gpu_metrics.vram_used_gb;
            system_metrics.npu_percent = ctx_.metrics->get_npu_utilization();

            res.set_content(build_prometheus_metrics(*ctx_.router, system_metrics),
                            "text/plain; version=0.0.4; charset=utf-8");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_metrics: " << e.what() << std::endl;
            res.status = kUnexpectedException.status;
            res.set_content("# Lemonade metrics error\n", "text/plain; version=0.0.4; charset=utf-8");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_metrics_route(ServerContext& ctx) {
    return std::make_unique<MetricsRoute>(ctx);
}

} // namespace lemon
