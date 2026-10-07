#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class AliasesListRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.aliases_list";
        s.methods = {"GET"};
        s.paths = {"aliases"};
        s.prefixes = Prefixes::Internal;
        s.summary = "List all active model aliases";
        s.description = "Lists every model alias with the model it points to.";
        s.notes = {
            "An alias stands in for its target in any request's `model` field. Aliases live in "
            "`<cache_dir>/aliases.json`; see "
            "[Model Aliases](../guide/configuration/custom-models.md).",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["aliases"],
            "properties": {
                "aliases": {"type": "array", "items": {
                    "type": "object",
                    "required": ["alias", "target", "downloaded", "recipe"],
                    "properties": {
                        "alias": {"type": "string"},
                        "target": {"type": "string", "description": "Model or alias the alias points to."},
                        "downloaded": {"type": "boolean", "description": "Whether the target model is downloaded."},
                        "recipe": {"type": "string", "description": "The target model's recipe; llamacpp when the target is not in the registry."}
                    }
                }}
            }
        })");
        response.setup = {{"internal.aliases_create", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            json alias_list = json::array();
            for (const auto& [alias, target] : ctx_.alias_manager->get_all_aliases()) {
                bool downloaded = false;
                std::string recipe = "llamacpp";
                try {
                    if (ctx_.model_manager->model_exists(target)) {
                        std::string canonical = ctx_.model_manager->resolve_model_name(target);
                        downloaded = ctx_.model_manager->is_model_downloaded(canonical);
                        ModelInfo info = ctx_.model_manager->get_model_info(canonical);
                        recipe = info.recipe;
                    }
                } catch (...) {}
                alias_list.push_back({
                    {"alias", alias},
                    {"target", target},
                    {"downloaded", downloaded},
                    {"recipe", recipe}
                });
            }
            res.set_content(json{{"aliases", alias_list}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json{{"error", {
                {"message", e.what()},
                {"type", "server_error"},
                {"param", "alias"},
                {"code", "internal_error"}
            }}}.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_list_route(ServerContext& ctx) {
    return std::make_unique<AliasesListRoute>(ctx);
}

} // namespace lemon
