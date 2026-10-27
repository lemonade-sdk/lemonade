#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class ModelsIdRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id";
        s.methods = {"GET"};
        s.paths = {"models/{id}"};
        s.summary = "Read one model";
        s.modality = "n/a";
        s.description =
            "Returns one model, as the same model object [`GET /v1/models`](#get-v1models) lists.";
        s.args = {openai::models::kId, openai::models::kIncludePaths};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ModelJson::schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "Qwen3-0.6B-GGUF"}};
        s.examples = {example};
        s.errors = {ModelLoader::kModelNotSupported, ModelLoader::kModelNotFound};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const std::string model_id =
            utils::normalize_model_name(req.value(openai::models::kId, std::string()));

        if (auto resolved = ctx_.model_manager->resolve(model_id)) {
            res.set_content(ctx_.model_json
                                ->to_json(resolved->wire_id, *resolved->info, 0,
                                          req.value(openai::models::kIncludePaths, false))
                                .dump(),
                            "application/json");
            return;
        }

        ctx_.model_loader->write_load_error(res, model_id, "Model not found");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdRoute>(ctx);
}

} // namespace lemon
