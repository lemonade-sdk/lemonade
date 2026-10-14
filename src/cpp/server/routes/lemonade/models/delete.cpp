#include <chrono>
#include <thread>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "An unknown model, or one this system does not support, answers `422`; other failures "
            "answer `500`.",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "[Lemonade Server model name](https://lemonade-server.ai/models.html) to delete. "
             "`model` is accepted as an alias."},
        };

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
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;
        try {
            // Accept both "model" and "model_name" for compatibility
            std::string model_name = request_json.contains("model") ?
                request_json["model"].get<std::string>() :
                request_json["model_name"].get<std::string>();

            LOG(INFO, "Server") << "Deleting model: " << model_name << std::endl;

            // Unload first so the backend releases its file locks.
            if (ctx_.router->is_model_loaded(model_name)) {
                LOG(INFO, "Server") << "Model is loaded, unloading before delete: " << model_name << std::endl;
                ctx_.router->unload_model(model_name);
            }

            // A download cancelled just before this request may not have released its
            // file handles yet, so a file-in-use failure is retried.
            const int max_retries = 3;
            const int retry_delay_seconds = 5;
            std::string last_error;

            for (int attempt = 0; attempt <= max_retries; ++attempt) {
                try {
                    ctx_.model_manager->delete_model(model_name);

                    json response = {
                        {"status", "success"},
                        {"message", "Deleted model: " + model_name}
                    };
                    res.set_content(response.dump(), "application/json");
                    return;

                } catch (const std::exception& e) {
                    last_error = e.what();

                    bool is_file_locked =
                        last_error.find("being used by another process") != std::string::npos ||
                        last_error.find("Permission denied") != std::string::npos ||
                        last_error.find("resource busy") != std::string::npos;

                    if (is_file_locked && attempt < max_retries) {
                        LOG(INFO, "Server") << "Delete failed (file in use), retry "
                                  << (attempt + 1) << "/" << max_retries
                                  << " in " << retry_delay_seconds << "s..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                        continue;
                    }

                    throw;
                }
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_delete: " << e.what() << std::endl;

            std::string error_msg = e.what();
            if (error_msg.find("Model not found") != std::string::npos ||
                error_msg.find("not supported") != std::string::npos) {
                res.status = 422;
            } else {
                res.status = 500;
            }

            json error = {{"error", e.what()}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_delete_route(ServerContext& ctx) {
    return std::make_unique<DeleteRoute>(ctx);
}

} // namespace lemon
