#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class TagsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.tags";
        s.methods = {"GET"};
        s.paths = {"/api/tags"};
        s.prefixes = Prefixes::Root;
        s.summary = "List downloaded models";
        s.description = "Lists the downloaded models in Ollama's format.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["models"],
            "properties": {
                "models": {"type": "array", "description": "The downloaded models.", "items": {
                    "type": "object",
                    "required": ["name", "model", "modified_at", "size", "digest", "details"],
                    "properties": {
                        "name": {"type": "string", "description": "Lemonade model name with a `:latest` tag."},
                        "model": {"type": "string", "description": "Same as `name`."},
                        "modified_at": {"type": "string", "description": "Always `2024-01-01T00:00:00Z`."},
                        "size": {"type": "integer", "description": "Size in bytes, from the registry's estimate."},
                        "digest": {"type": "string", "description": "Always an all-zero sha256."},
                        "details": {}
                    }
                }}
            }
        })");
        response.schema["properties"]["models"]["items"]["properties"]["details"] = ollama_details_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {ApiRoute::kUnexpectedException};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            auto models = ctx_.model_manager->get_downloaded_models();

            json response;
            response["models"] = json::array();

            for (const auto& [id, info] : models) {
                // info.size is in GB (2^30 bytes).
                int64_t size_bytes = static_cast<int64_t>(info.size * 1073741824.0);
                response["models"].push_back({
                    {"name", id + ":latest"},
                    {"model", id + ":latest"},
                    {"modified_at", "2024-01-01T00:00:00Z"},
                    {"size", size_bytes},
                    {"digest", "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
                    {"details", ollama_details(id, info.recipe, info.checkpoint())}
                });
            }

            res.set_content(response.dump(), "application/json");

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/tags: " << e.what() << std::endl;
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_tags_route(ServerContext& ctx) {
    return std::make_unique<TagsRoute>(ctx);
}

} // namespace lemon
