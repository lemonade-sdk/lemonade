#include "../../telemetry.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class TelemetryFlushRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.telemetry_flush";
        s.methods = {"POST"};
        s.paths = {"telemetry/flush"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Force-flush all queued telemetry trace spans";
        s.description =
            "Sends every queued trace span to the configured OTLP collector now, and answers "
            "once they are serialized and sent.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "flushed"}
            }
        })");
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        lemon::telemetry::flush();
        res.status = 200;
        res.set_content(json{{"status", "flushed"}}.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_telemetry_flush_route(ServerContext& ctx) {
    return std::make_unique<TelemetryFlushRoute>(ctx);
}

} // namespace lemon
