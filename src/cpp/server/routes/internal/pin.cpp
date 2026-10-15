#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidJson{400, "", "The body is not valid JSON"};
const RouteError kMissingModel{400, "", "The body has neither `model` nor `model_name`"};
const RouteError kBadPinned{400, "", "`pinned` is missing or not a boolean"};
const RouteError kNotLoaded{400, "", "`model_name` is not a string or names no loaded model"};

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
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Loaded model to pin or unpin. `model` is accepted in its place and wins when both are sent."},
            {"pinned", ArgIn::JsonBody, {{"type", "boolean"}}, true, Support::Available,
             "`true` pins the model; `false` unpins it."},
        };

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
        s.errors = {kInvalidJson, kMissingModel, kBadPinned, kNotLoaded};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            json request_json;
            try {
                request_json = json::parse(req.http.body);
            } catch (const std::exception& parse_err) {
                write_openai_error(res, kInvalidJson,
                                   "Invalid JSON body: " + std::string(parse_err.what()));
                return;
            }

            if (!request_json.contains("model") && !request_json.contains("model_name")) {
                write_openai_error(res, kMissingModel, "Parameter 'model' or 'model_name' is required");
                return;
            }

            if (!request_json.contains("pinned") || !request_json["pinned"].is_boolean()) {
                write_openai_error(res, kBadPinned,
                                   "Parameter 'pinned' is required and must be a boolean");
                return;
            }

            std::string model_name = request_json.contains("model") ?
                request_json["model"].get<std::string>() :
                request_json["model_name"].get<std::string>();
            bool pinned = request_json["pinned"].get<bool>();

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
