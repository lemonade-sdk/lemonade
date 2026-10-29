#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_registration.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kRegisterFailed{
    500, "",
    "The definition is invalid in another way and nothing is registered, for example a GGUF "
    "`checkpoint` without a variant, a recipe this system cannot run, or labels that name two "
    "deployment modes"};

const RouteArg kModelName{"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                          "Non-empty namespaced model name under `user.*`."};
const RouteArg kRecipe{"recipe", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                       "Recipe that loads the model, such as `llamacpp`."};
const RouteArg kCheckpoint{"checkpoint", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                           "Main checkpoint. Model types with no local weights have none."};
const RouteArg kCheckpoints{"checkpoints", ArgIn::JsonBody,
                            {{"type", "object"}, {"additionalProperties", {{"type", "string"}}}}, false, Support::Available,
                            "Named checkpoints for multi-checkpoint models."};
const RouteArg kSource{"source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "Registry or local source. Remote values are `huggingface` or `modelscope`."};
const RouteArg kLabels{"labels", ArgIn::JsonBody,
                       {{"type", "array"}, {"items", {{"type", "string"}}}}, false, Support::Available,
                       "Model labels; see [Model Labels](./openai.md#model-labels)."};
const RouteArg kComponents{"components", ArgIn::JsonBody,
                           {{"type", "array"}, {"items", {{"type", "string"}}}}, false, Support::Available,
                           "Collections only: the component model names, each already registered."};

class RegisterRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.models_register";
        s.methods = {"POST"};
        s.paths = {"models/register"};
        s.summary = "Register or update a user model definition without downloading it";
        s.description =
            "Registers or updates a `user.*` model definition in `user_models.json` without "
            "downloading any files, for clients that register and install as separate steps. "
            "[`POST /v1/pull`](#post-v1pull) registers the same way and then downloads.";
        s.args = {kModelName, kRecipe, kCheckpoint, kCheckpoints, kSource, kLabels, kComponents};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"status", "model_name", "canonical_model_name"})},
            {"properties", {
                {"status", {{"const", "success"}, {"description", "Always `success`."}}},
                {"model_name", {{"type", "string"}, {"description", "The ID [`GET /v1/models`](./openai.md#get-v1models) lists for the model."}}},
                {"canonical_model_name", {{"type", "string"}, {"description", "The `user.*` name the request registered."}}},
                {"model", [] {
                    ordered_json model = ModelJson::schema();
                    model["description"] =
                        "The registered model, in the shape "
                        "[`GET /v1/models/{id}`](./openai.md#get-v1modelsid) returns.";
                    return model;
                }()},
            }},
        };
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model_name": "user.Phi-4-Mini-GGUF",
            "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
            "recipe": "llamacpp"
        })");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kInvalidModelDefinition, kUnlistedFieldInvalid,
                    kRegisterFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            const std::string model_name = req.arg(kModelName).get<std::string>();
            const auto registered = try_register_model_definition(
                ctx_,
                model_name,
                req.body,
                /*require_definition=*/true,
                /*allow_embedded_models=*/false,
                /*local_import=*/false,
                res);
            if (!registered) {
                return;
            }
            const std::string& public_name = *registered;

            json response = {
                {"status", "success"},
                {"model_name", public_name},
                {"canonical_model_name", model_name},
            };

            const auto models = ctx_.model_manager->get_supported_models();
            auto model_it = models.find(public_name);
            if (model_it != models.end()) {
                response["model"] = ctx_.model_json->to_json(public_name, model_it->second);
            }

            res.set_content(response.dump(), "application/json");
        } catch (const json::exception& e) {
            write_plain_error(res, kUnlistedFieldInvalid, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_model_register: " << e.what() << std::endl;
            write_plain_error(res, kRegisterFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_register_route(ServerContext& ctx) {
    return std::make_unique<RegisterRoute>(ctx);
}

} // namespace lemon
