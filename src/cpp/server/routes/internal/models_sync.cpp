#include <memory>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {

using json = nlohmann::json;

namespace {

class ModelsSyncRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.models_sync";
        s.methods = {"POST"};
        s.paths = {"models/sync"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Download updates for downloaded models";
        s.description =
            "Checks downloaded models for newer upstream commits and downloads the updates, by "
            "default in the background.";
        s.notes = {
            "With `async` omitted or `true`, the sync starts in the background and the answer is "
            "`202` with the sync's status; poll [`GET /internal/models/sync/status`]"
            "(#get-internalmodelssyncstatus) with its `sync_id`. With `async: false`, the answer "
            "waits for the sync to finish. `dry_run: true` only checks, and answers `200` with "
            "what a sync would update.",
            "A sync already running answers with its status instead of starting another. Full "
            "offline mode (`offline=true`) answers `409` without any network request.",
            "The `lemonade update-models` command uses this endpoint; see "
            "Model Synchronization & Auto-Updates in [Server Configuration](../guide/configuration/README.md).",
        };
        s.args = {
            {"models", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}}, false, Support::Available,
             "Models to sync. Defaults to every downloaded model."},
            {"dry_run", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Check for updates without downloading them. Defaults to `false`."},
            {"async", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Run in the background and answer immediately. Defaults to `true`."},
            {"attach_if_running", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Add the models to a sync that is already running instead of reporting it. "
             "Defaults to `false`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ModelManager::sync_status_schema();
        response.example = {{"models", json::array({"Qwen3-0.6B-GGUF"})}, {"dry_run", true}};
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (ctx_.config->offline()) {
            write_plain_error(res, 409, "Cannot sync model updates while offline=true");
            return;
        }

        std::vector<std::string> target_models;
        bool dry_run = false;
        bool async_dispatch = true;
        bool async_specified = false;
        bool attach_if_running = false;

        if (!req.http.body.empty()) {
            try {
                auto body_json = json::parse(req.http.body);
                if (!body_json.is_object()) {
                    write_plain_error(res, 400, "Request body must be a JSON object");
                    return;
                }
                if (body_json.contains("models")) {
                    if (!body_json["models"].is_array()) {
                        write_plain_error(res, 400, "'models' must be an array of strings");
                        return;
                    }
                    for (const auto& item : body_json["models"]) {
                        if (!item.is_string()) {
                            write_plain_error(res, 400, "Each item in 'models' must be a string");
                            return;
                        }
                        target_models.push_back(item.get<std::string>());
                    }
                }
                if (body_json.contains("dry_run")) {
                    if (!body_json["dry_run"].is_boolean()) {
                        write_plain_error(res, 400, "'dry_run' must be a boolean");
                        return;
                    }
                    dry_run = body_json["dry_run"].get<bool>();
                }
                if (body_json.contains("async")) {
                    if (!body_json["async"].is_boolean()) {
                        write_plain_error(res, 400, "'async' must be a boolean");
                        return;
                    }
                    async_dispatch = body_json["async"].get<bool>();
                    async_specified = true;
                }
                if (body_json.contains("attach_if_running")) {
                    if (!body_json["attach_if_running"].is_boolean()) {
                        write_plain_error(res, 400, "'attach_if_running' must be a boolean");
                        return;
                    }
                    attach_if_running = body_json["attach_if_running"].get<bool>();
                }
            } catch (const std::exception& e) {
                write_plain_error(res, 400, std::string("Invalid JSON body: ") + e.what());
                return;
            }
        }

        ModelManager* model_manager = ctx_.model_manager;

        if (dry_run) {
            try {
                json sync_result = model_manager->sync_models(target_models, true);
                res.set_content(sync_result.dump(), "application/json");
                res.status = 200;
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Model sync dry-run failed: " << e.what() << std::endl;
                write_plain_error(res, 500, std::string("Model sync dry-run failed: ") + e.what());
            }
            return;
        }

        if (!async_specified || async_dispatch) {
            auto enqueue_res = model_manager->enqueue_sync(target_models, attach_if_running);
            if (enqueue_res.already_running) {
                json status = model_manager->get_sync_status(enqueue_res.sync_id);
                res.set_content(status.dump(), "application/json");
                res.status = 200;
                return;
            }

            model_manager->execute_sync_in_background();

            json dispatched_result = model_manager->get_sync_status(enqueue_res.sync_id);
            dispatched_result["message"] = "Model synchronization dispatched in background";
            dispatched_result["async"] = true;
            dispatched_result["already_in_progress"] = false;
            res.set_content(dispatched_result.dump(), "application/json");
            res.status = 202;
            return;
        }

        try {
            json sync_result = model_manager->sync_models(target_models, false, attach_if_running);
            res.set_content(sync_result.dump(), "application/json");
            res.status = 200;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Model sync failed: " << e.what() << std::endl;
            write_plain_error(res, 500, std::string("Model sync failed: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_sync_route(ServerContext& ctx) {
    return std::make_unique<ModelsSyncRoute>(ctx);
}

} // namespace lemon
