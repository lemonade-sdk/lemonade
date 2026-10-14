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
            "Prometheus scrape endpoint for Lemonade Server. The endpoint returns Prometheus text "
            "exposition format and is intended to be scraped by Prometheus, not by Grafana "
            "directly.\n\n"
            "Unlike most Lemonade API endpoints, `/metrics` is root-level only. It is not mounted "
            "under `/api/v0/`, `/api/v1/`, `/v0/`, or `/v1/`.\n\n"
            "`HEAD /metrics` is also supported and returns `200 OK` with an empty body.";
        s.notes = {
            "### Authentication\n\n"
            "If `LEMONADE_API_KEY` is set, `/metrics` requires bearer authentication. Either the "
            "regular API key or `LEMONADE_ADMIN_API_KEY` is accepted.\n\n"
            "If only `LEMONADE_ADMIN_API_KEY` is set and `LEMONADE_API_KEY` is unset, `/metrics` "
            "is accessible without authentication, matching regular API endpoint behavior.",
            "### Polling and Refresh Rate\n\n"
            "The `/metrics` endpoint has no internal refresh timer. It renders the latest server "
            "state at the moment it is scraped.\n\n"
            "Polling frequency is configured in Prometheus via `scrape_interval`, for example:\n\n"
            "```yaml\n"
            "global:\n"
            "  scrape_interval: 10s\n"
            "```\n\n"
            "Grafana queries Prometheus. Grafana's dashboard refresh controls how often panels "
            "query Prometheus, but it does not control how often Prometheus scrapes Lemonade.",
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
