#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_registration.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kUnknownModel{
    422, "", "The model is not in the registry, or this system does not support it"};

class DeleteRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.delete";
        s.methods = {"POST"};
        s.paths = {"delete"};
        s.summary = "Delete a model";
        s.description =
            "Deletes a model's files from local storage, unloading the model first when it is "
            "loaded. A `user.*` model's entry in `user_models.json` is removed too. Deleting a "
            "collection (`recipe: \"collection.omni\"`) removes only that entry; delete its "
            "components to free their disk space.";
        s.notes = {
            "A file still held open, for example by a download that was just cancelled, is retried "
            "up to 3 times, 5 seconds apart.",
        };
        s.args = {model_management::kModelName};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "message"],
            "properties": {
                "status": {"const": "success", "description": "Always `success`."},
                "message": {"type": "string", "description": "A confirmation naming the deleted model."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.models_register", ""}};
        example.args = {{"model_name", "$lemonade.models_register/canonical_model_name"}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kUnknownModel, kDeleteFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model_name = req.arg(model_management::kModelName).get<std::string>();

            LOG(INFO, "Server") << "Deleting model: " << model_name << std::endl;

            delete_model_files(ctx_, model_name);
            json response = {
                {"status", "success"},
                {"message", "Deleted model: " + model_name}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_delete: " << e.what() << std::endl;

            write_delete_error(e, kUnknownModel, res);
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_delete_route(ServerContext& ctx) {
    return std::make_unique<DeleteRoute>(ctx);
}

} // namespace lemon
