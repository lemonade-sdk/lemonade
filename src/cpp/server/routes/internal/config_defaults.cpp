#include <lemon/utils/aixlog.hpp>

#include "lemon/config_file.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kDefaultsUnreadable{500, "", "`resources/defaults.json` is missing or is not valid JSON"};

class ConfigDefaultsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.config_defaults";
        s.methods = {"GET"};
        s.paths = {"config/defaults"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Read the factory default config";
        s.description =
            "Returns the default configuration built into this release, independent of this "
            "instance's `config.json` and of any deployment override. "
            "`docs/tools/gen_backend_boilerplate.py` reads it to regenerate "
            "`src/cpp/resources/defaults.json`.";
        s.notes = {
            "The per-backend sections come from the backend descriptors, so this is the "
            "authoritative list of factory defaults.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "description": "Every `config.json` key with its factory default.",
            "additionalProperties": {
                "description": "A setting; see the Settings Reference."
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kDefaultsUnreadable};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            res.set_content(ConfigFile::base_defaults().dump(2), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in /internal/config/defaults: " << e.what() << std::endl;
            write_plain_error(res, kDefaultsUnreadable, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_config_defaults_route(ServerContext& ctx) {
    return std::make_unique<ConfigDefaultsRoute>(ctx);
}

} // namespace lemon
