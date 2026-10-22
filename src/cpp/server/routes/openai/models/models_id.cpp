#include "lemon/alias_manager.h"
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
        s.args = {openai::models::kId};

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
        ModelManager& model_manager = *ctx_.model_manager;
        AliasManager* alias_manager = ctx_.alias_manager;
        std::string model_id = utils::normalize_model_name(req.value(openai::models::kId, std::string()));

        if (model_manager.model_exists(model_id)) {
            auto info = model_manager.get_model_info(model_id);
            std::string canonical_cache_key = model_manager.resolve_model_name(model_id);
            std::string wire_id = model_manager.get_public_model_name(canonical_cache_key);
            if (alias_manager && alias_manager->has_alias(model_id)) {
                wire_id = model_id;
            }
            res.set_content(ctx_.model_json->to_json(wire_id, info).dump(), "application/json");
            return;
        }

        if (alias_manager && alias_manager->has_alias(model_id)) {
            auto resolved_target = alias_manager->resolve_alias(model_id);
            if (resolved_target) {
                std::string canonical_target = model_manager.resolve_model_name(*resolved_target);
                if (model_manager.model_exists(canonical_target)) {
                    auto info = model_manager.get_model_info(canonical_target);
                    res.set_content(ctx_.model_json->to_json(model_id, info).dump(), "application/json");
                    return;
                } else if (model_manager.model_exists(*resolved_target)) {
                    auto info = model_manager.get_model_info(*resolved_target);
                    res.set_content(ctx_.model_json->to_json(model_id, info).dump(), "application/json");
                    return;
                }
            }
        }

        ctx_.model_loader->write_load_error(res, model_id, "Model not found");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdRoute>(ctx);
}

} // namespace lemon
