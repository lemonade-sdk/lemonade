#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_registration.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Register or update a `user.*` model definition without downloading model files.\n"
            "Use this endpoint when registration and installation are separate actions.\n"
            "`POST /v1/pull` remains the install/download path and performs the same internal\n"
            "registration step before downloading.";
        s.notes = {
            "A checkpoint is intentionally not universally required: registration is a model\n"
            "metadata operation and some present or future model types may not have local\n"
            "weights. `/pull` remains the operation that attempts installation/download.",
            "The endpoint accepts one model definition. An embedded `models` array represents\n"
            "multiple definitions and remains a collection-import concern; register those\n"
            "component definitions first when using this endpoint.",
            "A `400` answers a body that is not a JSON object, a missing `model_name`, a name "
            "outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, a "
            "missing `recipe`, and malformed `checkpoint`, `checkpoints`, `source` or "
            "`components` values.",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Non-empty namespaced model name under `user.*`."},
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Lemonade recipe associated with the model definition."},
            {"checkpoint", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Main checkpoint, when the recipe uses one."},
            {"checkpoints", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Named checkpoints for multi-checkpoint models."},
            {"source", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Registry or local source. Remote values are `huggingface` or `modelscope`."},
            {"labels", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Additional model labels."},
            {"components", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Already-registered component model names for collection recipes."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"status", "model_name", "canonical_model_name"})},
            {"properties", {
                {"status", {{"const", "success"}}},
                {"model_name", {{"type", "string"}}},
                {"canonical_model_name", {{"type", "string"}}},
                {"model", ModelJson::schema()},
            }},
        };
        response.example = ordered_json::parse(R"({
            "model_name": "user.Phi-4-Mini-GGUF",
            "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
            "recipe": "llamacpp"
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        json& request_json = req.body;
        try {
            if (!request_json.is_object()) {
                write_plain_error(res, 400, "Request body must be a JSON object");
                return;
            }
            if (!request_json.contains("model_name") || !request_json["model_name"].is_string()) {
                write_plain_error(res, 400, "A string `model_name` is required");
                return;
            }

            const std::string model_name = request_json["model_name"].get<std::string>();
            const std::string public_name = register_model_definition(
                ctx_,
                model_name,
                request_json,
                /*require_definition=*/true,
                /*allow_embedded_models=*/false,
                /*local_import=*/false);

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
        } catch (const std::invalid_argument& e) {
            write_plain_error(res, 400, e.what());
        } catch (const json::exception& e) {
            write_plain_error(res, 400, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_model_register: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_register_route(ServerContext& ctx) {
    return std::make_unique<RegisterRoute>(ctx);
}

} // namespace lemon
