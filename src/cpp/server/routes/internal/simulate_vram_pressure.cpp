#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class SimulateVramPressureRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.simulate_vram_pressure";
        s.methods = {"POST"};
        s.paths = {"simulate-vram-pressure"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Run the eviction engine at a simulated VRAM usage";
        s.description =
            "Hands the eviction engine a simulated global VRAM usage, so tests can exercise "
            "pressure eviction without filling a GPU.";
        s.notes = {
            "Only unpinned models with `auto_evict` enabled are candidates. A `pct` at or above "
            "`auto_evict_threshold_pct` runs the idle-timeout checks and then unloads at most one "
            "model for pressure, `-1` runs only the idle-timeout checks, and any other value does "
            "nothing.",
            "The answer comes after the evaluation finishes, so a test can check the outcome "
            "right away. While an exclusive job session is active no evaluation runs, and the "
            "answer is still `200`.",
            "A `400` answers invalid JSON, a body that is not an object, or a non-numeric `pct`.",
        };
        s.args = {
            {"pct", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Simulated fraction of VRAM in use, such as `0.95`. Defaults to `0`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "ok", "description": "Always `ok`."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"pct", 0.5}};
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto req_json = json::parse(req.http.body);
            double pct = req_json.value("pct", 0.0);
            ctx_.router->simulate_vram_pressure(pct);
            res.set_content(R"({"status": "ok"})", "application/json");
        } catch (const std::exception& e) {
            write_plain_error(res, 400, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_simulate_vram_pressure_route(ServerContext& ctx) {
    return std::make_unique<SimulateVramPressureRoute>(ctx);
}

} // namespace lemon
