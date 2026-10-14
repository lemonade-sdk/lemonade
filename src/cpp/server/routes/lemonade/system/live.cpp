#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "[`/v1/health`](#get-v1health), this endpoint does no work beyond confirming the "
            "process is up — it does not inspect loaded models or backends — so it is safe to poll "
            "at high frequency. `HEAD /live` is also supported and returns `200 OK` with an empty "
            "body.\n\n"
            "Unlike most other endpoints on this page, `/live` is not versioned and is not mounted "
            "under the `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/` prefixes.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
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
