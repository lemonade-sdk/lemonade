#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class LiveRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.live";
        s.methods = {"GET"};
        s.paths = {"/live"};
        s.prefixes = Prefixes::Root;
        s.summary = "Check server liveness for load balancers and orchestrators";
        s.description =
            "Lightweight liveness probe for load balancers and orchestrators. Unlike "
            "[`/v1/health`](#get-v1health), it does no work beyond confirming the process is up "
            "and does not inspect loaded models or backends, so it is safe to poll at high "
            "frequency.";
        s.notes = {
            "`/live` is not versioned: it is not mounted under `/api/v0/`, `/api/v1/`, `/v0/` "
            "or `/v1/`. `HEAD /live` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "ok"}}
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
        res.set_content(R"({"status":"ok"})", "application/json");
        res.status = 200;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_live_route(ServerContext& ctx) {
    return std::make_unique<LiveRoute>(ctx);
}

} // namespace lemon
