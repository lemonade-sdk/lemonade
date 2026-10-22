#include "../../telemetry.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Sends every queued telemetry trace span to the configured OTLP collector, and "
            "answers once the queue is empty.";
        s.notes = {
            "A batch the collector rejects or does not receive is dropped without further "
            "retries, and the answer is still `flushed`.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "flushed", "description": "Always `flushed`."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
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
