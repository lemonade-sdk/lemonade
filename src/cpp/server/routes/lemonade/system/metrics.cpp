#include <lemon/utils/aixlog.hpp>

#include "lemon/prometheus_metrics.h"
#include "lemon/router.h"
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
        s.summary = "Prometheus metrics scrape endpoint";
        s.description =
            "Returns Lemonade, model, backend and system metrics in the Prometheus text "
            "exposition format (`text/plain; version=0.0.4; charset=utf-8`), for Prometheus to "
            "scrape; Grafana queries Prometheus rather than this endpoint.";
        s.notes = {
            "Unlike most Lemonade endpoints, `/metrics` is root-level only: it is not mounted "
            "under `/api/v0/`, `/api/v1/`, `/v0/` or `/v1/`. `HEAD /metrics` returns `200 OK` "
            "with an empty body.",
            "**Authentication:** when `LEMONADE_API_KEY` is set, `/metrics` requires bearer "
            "authentication with either that key or `LEMONADE_ADMIN_API_KEY`. When only "
            "`LEMONADE_ADMIN_API_KEY` is set, `/metrics` needs no key, like the regular API "
            "endpoints.",
            "**Metric families:** the names, types, labels and help text are defined by the "
            "`metrics.describe(...)` calls in "
            "[`src/cpp/server/prometheus_metrics.cpp`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/server/prometheus_metrics.cpp). "
            "Unsupported, unavailable, null, NaN and infinite values are omitted rather than "
            "emitted as samples.",
            "**llama.cpp backend metrics:** Lemonade starts llama.cpp backends with metrics "
            "enabled and, for each loaded `llamacpp` model, makes a best-effort scrape of the "
            "backend's private `/metrics` endpoint. Those metrics are renamed under the "
            "`lemonade_llamacpp_*` prefix and labeled with the same model metadata as "
            "`lemonade_model_info`. A failed backend scrape does not fail the response.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Text;
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

        try {
            SystemMetrics system_metrics;
            system_metrics.cpu_percent = ctx_.metrics->get_cpu_usage();
            system_metrics.gpu_percent = ctx_.metrics->get_gpu_usage();
            system_metrics.vram_gb = ctx_.metrics->get_vram_usage_gb();
            system_metrics.npu_percent = ctx_.metrics->get_npu_utilization();

            res.set_content(build_prometheus_metrics(*ctx_.router, system_metrics),
                            "text/plain; version=0.0.4; charset=utf-8");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_metrics: " << e.what() << std::endl;
            res.status = 500;
            res.set_content("# Lemonade metrics error\n", "text/plain; version=0.0.4; charset=utf-8");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_metrics_route(ServerContext& ctx) {
    return std::make_unique<MetricsRoute>(ctx);
}

} // namespace lemon
