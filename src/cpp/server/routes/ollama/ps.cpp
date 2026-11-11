#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class PsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.ps";
        s.methods = {"GET"};
        s.paths = {"/api/ps"};
        s.prefixes = Prefixes::Root;
        s.summary = "List loaded models";
        s.description = "Lists the loaded models in Ollama's format.";
        s.notes = {
            "[`GET /v1/health`](./lemonade.md#get-v1health) reports the loaded models in full.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["models"],
            "properties": {
                "models": {"type": "array", "description": "The loaded models.", "items": {
                    "type": "object",
                    "required": ["name", "model", "size", "digest", "details", "expires_at", "size_vram"],
                    "properties": {
                        "name": {"type": "string", "description": "Lemonade model name with a `:latest` tag."},
                        "model": {"type": "string", "description": "Same as `name`."},
                        "size": {"type": "integer", "description": "Always `0`."},
                        "digest": {"type": "string", "description": "Always an all-zero sha256."},
                        "details": {},
                        "expires_at": {"type": "string", "description": "Always `2099-01-01T00:00:00Z`."},
                        "size_vram": {"type": "integer", "description": "Always `0`."}
                    }
                }}
            }
        })");
        response.schema["properties"]["models"]["items"]["properties"]["details"] = ollama_details_schema();
        // A loaded model makes the listing worth reading.
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", "model"}};
        example.args = json::object();
        s.examples = {example};
        s.errors = {ApiRoute::kUnexpectedException};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            auto loaded = ctx_.router->get_all_loaded_models();

            json response;
            response["models"] = json::array();

            if (loaded.is_array()) {
                for (const auto& m : loaded) {
                    std::string name = m.value("model_name", "");
                    std::string recipe = m.value("recipe", "");
                    std::string checkpoint = m.value("checkpoint", "");
                    json entry = {
                        {"name", name + ":latest"},
                        {"model", name + ":latest"},
                        {"size", 0},
                        {"digest", "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
                        {"details", ollama_details(name, recipe, checkpoint)},
                        {"expires_at", "2099-01-01T00:00:00Z"},
                        {"size_vram", 0}
                    };
                    response["models"].push_back(entry);
                }
            }

            res.set_content(response.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/ps: " << e.what() << std::endl;
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_ps_route(ServerContext& ctx) {
    return std::make_unique<PsRoute>(ctx);
}

} // namespace lemon
