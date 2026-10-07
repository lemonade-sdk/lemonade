#include <functional>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"

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
        s.summary = "Read a model's saved, effective, and default recipe options";
        s.description =
            "Lemonade extension: reads a model's recipe options, separated by layer, without "
            "loading it. With `POST` and `DELETE` on the same path, this manages per-model options "
            "independently of [`/v1/load`](./lemonade.md#post-v1load).";
        s.notes = {
            "`effective` is the exact request body a [`POST /v1/load`](./lemonade.md#post-v1load) "
            "for this model uses right now, with every option the recipe accepts resolved through "
            "the full priority chain. `defaults` is what a reset model would get.",
            "Per-architecture defaults come from the model's GGUF metadata. For a model that has "
            "not been downloaded yet, every key is still present but carries the value it has "
            "before those defaults apply.",
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
        respond_with_model_options(ctx_, req.http, res, nullptr);
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_options_get_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdOptionsGetRoute>(ctx);
}

} // namespace lemon
