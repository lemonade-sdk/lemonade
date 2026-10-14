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
            "Liveness probe for load balancers and orchestrators. Unlike "
            "[`/v1/health`](#get-v1health), it only confirms the process is up, without inspecting "
            "models or backends, so it is safe to poll at high frequency. `HEAD /live` returns "
            "`200 OK` with an empty body.\n\n"
            "`/live` is served only at the root, not under the versioned prefixes.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {"status": {"const": "ok"}}
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
        res.set_content(R"({"status":"ok"})", "application/json");
        res.status = 200;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_live_route(ServerContext& ctx) {
    return std::make_unique<LiveRoute>(ctx);
}

} // namespace lemon
