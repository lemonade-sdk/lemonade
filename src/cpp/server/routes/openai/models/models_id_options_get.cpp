#include <functional>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"
#include "lemon/server/shared_args.h"

namespace lemon {

namespace {

using json = nlohmann::json;

class ModelsIdOptionsGetRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_options_get";
        s.methods = {"GET"};
        s.paths = {"models/{id}/options"};
        s.summary = "Read a model's recipe options by layer";
        s.modality = "n/a";
        s.description =
            "Reads a model's recipe options, separated by layer, without loading it. With `POST` "
            "and `DELETE` on the same path, this manages per-model options independently of "
            "[`/v1/load`](./lemonade.md#post-v1load).";
        s.notes = {
            "Per-architecture defaults are chosen by the architecture in the model's GGUF "
            "metadata, so a model that has not been downloaded yet shows every key with the "
            "value it has before those defaults apply.",
        };
        s.args = {openai::models::kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_options_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "Qwen3-0.6B-GGUF"}};
        s.examples = {example};
        s.errors = model_options_errors();
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        respond_with_model_options(ctx_, req.value(openai::models::kId, std::string()), res, nullptr);
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_options_get_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdOptionsGetRoute>(ctx);
}

} // namespace lemon
