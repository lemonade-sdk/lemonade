#include <functional>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"
#include "lemon/server/shared_args.h"

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
        s.summary = "Reset a model's recipe options";
        s.modality = "n/a";
        s.description =
            "Resets a model to its defaults by erasing its `recipe_options.json` entry. Defaults "
            "from its registry entry and the server's configuration still apply. Clearing saved backend "
            "arguments requires [admin authorization](../guide/configuration/README.md#admin-api-key).";
        s.args = {openai::models::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_options_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"openai.models_id_options_post", ""}};
        example.args = {{"id", "Qwen3-4B-GGUF"}};
        s.examples = {example};
        s.errors = join_errors({model_options_errors(), {kBackendArgsAdminRequired}});
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager* model_manager = ctx_.model_manager;
        respond_with_model_options(ctx_, req.value(openai::models::kId, std::string()), res,
            [this, model_manager, &req](const std::string& model_key, ModelInfo& info, httplib::Response&) {
                if (!model_manager->options(model_key)["saved"].empty()) {
                    model_manager->replace_options(model_key, json::object(), is_admin_request(req));
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
