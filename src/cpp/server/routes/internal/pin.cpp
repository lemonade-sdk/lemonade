#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNotLoaded{400, "", "The model is not loaded"};

const RouteArg kPinned{"pinned", ArgIn::JsonBody, {{"type", "boolean"}}, true, Support::Available,
                       "`true` pins the model; `false` unpins it."};

class PinRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.pin";
        s.methods = {"POST"};
        s.paths = {"pin"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Pin or unpin a loaded model";
        s.description =
            "Pins or unpins a loaded model without reloading it. Least-recently-used eviction "
            "skips pinned models.";
        s.notes = {
            "A [`POST /v1/load`](./lemonade.md#post-v1load) or inference request that needs a "
            "slot held only by pinned models fails with a "
            "`slots_pinned_error` code; see [Model Pinning](../guide/configuration/multi-model.md#model-pinning).",
        };
        s.args = {model_management::kModelName, kPinned};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "model_name", "pinned"],
            "properties": {
                "status": {"const": "success", "description": "Always `success`."},
                "model_name": {"type": "string", "description": "The model name as sent."},
                "pinned": {"type": "boolean", "description": "The model's pinned state now."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", "model"}};
        example.args = {{"model_name", "Qwen3-0.6B-GGUF"}, {"pinned", true}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kNotLoaded};
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model_name = req.arg(model_management::kModelName).get<std::string>();
            bool pinned = req.arg(kPinned).get<bool>();

            std::string canonical_name = ctx_.model_manager->resolve_model_name(model_name);
            ctx_.router->set_model_pinned(canonical_name, pinned);

            json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"pinned", pinned}
            };
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Pin/unpin failed: " << e.what() << std::endl;
            write_openai_error(res, kNotLoaded, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_pin_route(ServerContext& ctx) {
    return std::make_unique<PinRoute>(ctx);
}

} // namespace lemon
