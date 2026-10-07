#include <functional>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"

namespace lemon {

namespace {

using json = nlohmann::json;

class ModelsIdOptionsDeleteRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_options_delete";
        s.methods = {"DELETE"};
        s.paths = {"models/{id}/options"};
        s.summary = "Reset a model's recipe options to defaults";
        s.description =
            "Lemonade extension: resets a model to its defaults by erasing its "
            "`recipe_options.json` entry. The response is the same as "
            "[`GET /v1/models/{id}/options`](#get-v1modelsidoptions), with `saved` now `{}`.";
        s.notes = {
            "The model keeps the defaults that come from its registry entry and from the "
            "server's global configuration; only the saved overrides are removed.",
        };
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_options_schema();
        response.setup = {{"openai.models_id_options_post", ResponseFormat::Json}};
        response.example = {{"id", "Qwen3-4B-GGUF"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager* model_manager = ctx_.model_manager;
        respond_with_model_options(ctx_, req.http, res,
            [model_manager](const std::string& model_key, ModelInfo& info, httplib::Response&) {
                if (!model_manager->get_saved_model_options(model_key).empty()) {
                    model_manager->set_saved_model_options(model_key, json::object());
                }
                info = model_manager->get_model_info(model_key);
                return true;
            });
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_options_delete_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdOptionsDeleteRoute>(ctx);
}

} // namespace lemon
