#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kPct{"pct", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                    "Simulated fraction of VRAM in use, such as `0.95`. Defaults to `0`."};

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
            "Only unpinned models with `auto_evict` enabled are candidates, and a model in use is "
            "never chosen. One evaluation unloads at most one model: the first, in load order, past "
            "its `evict_idle_timeout`, or when there is none and `pct` is at or above "
            "`auto_evict_threshold_pct`, the one with the highest [eviction "
            "score](../guide/configuration/multi-model.md#dynamic-vram-management-opt-in). It also "
            "downsizes ready models past their `downsize_idle_timeout`. `-1` runs only the "
            "idle-timeout checks, and any other value below the threshold does nothing.",
            "The answer comes after the evaluation finishes, so a test can check the outcome "
            "right away. While an exclusive job session is active no evaluation runs, and the "
            "answer is still `200`.",
        };
        s.args = {kPct};

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
        s.errors = {kBodyNotJson, kArgInvalid};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        double pct = req.value(kPct, 0.0);
        ctx_.router->simulate_vram_pressure(pct);
        res.set_content(R"({"status": "ok"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_simulate_vram_pressure_route(ServerContext& ctx) {
    return std::make_unique<SimulateVramPressureRoute>(ctx);
}

} // namespace lemon
