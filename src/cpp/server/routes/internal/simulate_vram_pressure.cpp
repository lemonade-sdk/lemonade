#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Runs one eviction-engine evaluation as if global VRAM usage were `pct`, so tests "
            "can exercise pressure eviction without filling a GPU.";
        s.notes = {
            "Only unpinned models with `auto_evict` enabled are candidates. A `pct` at or above "
            "`auto_evict_threshold_pct` runs pressure eviction, `-1` runs only the idle-timeout "
            "checks, and any other value does nothing.",
        };
        s.args = {
            {"pct", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Simulated fraction of VRAM in use, e.g. `0.95`. Defaults to `0`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "ok"}
            }
        })");
        response.example = {{"pct", 0.5}};
        s.responses = {response};
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
