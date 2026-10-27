#include "alias_errors.h"
#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


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
        s.description = "Lists every model alias with its target.";
        s.notes = {
            "An alias stands in for its target in the `model` field of inference requests and of "
            "[`POST /v1/load`](./lemonade.md#post-v1load) and "
            "[`POST /v1/unload`](./lemonade.md#post-v1unload); the Ollama and Anthropic gateways "
            "do not resolve aliases. "
            "Aliases live in "
            "`<cache_dir>/aliases.json`; see "
            "[Model Aliases](../guide/configuration/custom-models.md).",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["aliases"],
            "properties": {
                "aliases": {"type": "array", "description": "Each alias with its target.", "items": {
                    "type": "object",
                    "required": ["alias", "target", "downloaded", "recipe"],
                    "properties": {
                        "alias": {"type": "string", "description": "The alias name."},
                        "target": {"type": "string", "description": "Model or alias the alias points to."},
                        "downloaded": {"type": "boolean", "description": "Whether the target model is downloaded."},
                        "recipe": {"type": "string", "description": "The target model's recipe; `llamacpp` when the target is not in the registry."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"internal.aliases_create", ""}};
        example.args = json::object();
        s.examples = {example};
        s.errors = {kAliasStoreFailed};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            json alias_list = json::array();
            if (ctx_.alias_manager) {
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
            }
            res.set_content(json{{"aliases", alias_list}}.dump(), "application/json");
        } catch (const std::exception& e) {
            write_alias_error(res, kAliasStoreFailed, e.what(), "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_list_route(ServerContext& ctx) {
    return std::make_unique<AliasesListRoute>(ctx);
}

} // namespace lemon
