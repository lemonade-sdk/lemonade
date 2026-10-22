#include <lemon/utils/aixlog.hpp>

#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


class ConfigRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.config";
        s.methods = {"GET"};
        s.paths = {"config"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Read the runtime config";
        s.description =
            "Returns the full runtime configuration: every `config.json` key, server-level and "
            "per-backend, with its current value. The `lemonade config` command reads it.";
        s.notes = {
            "See the "
            "[Settings Reference](../guide/configuration/README.md#settings-reference) for "
            "each key.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "description": "Every `config.json` key with its current value.",
            "additionalProperties": {
                "description": "A setting; see the Settings Reference."
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kUnexpectedException};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            res.set_content(ctx_.config->snapshot().dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in /internal/config: " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_config_route(ServerContext& ctx) {
    return std::make_unique<ConfigRoute>(ctx);
}

} // namespace lemon
