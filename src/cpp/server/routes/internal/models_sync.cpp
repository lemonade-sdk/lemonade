#include <memory>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_sync.h"

namespace lemon {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {

const RouteError kOffline{409, "", "Offline mode (`offline=true`) is on, so no network request is made"};
const RouteError kDryRunFailed{500, "", "Checking for updates with `dry_run` fails"};
const RouteError kSyncFailed{500, "", "A sync with `async: false` fails"};

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
            "Checks downloaded models for newer upstream commits and downloads the updates. The "
            "`lemonade update-models` command uses it.";
        s.notes = {
            "A background sync answers `202` with its status; poll "
            "[`GET /internal/models/sync/status`](#get-internalmodelssyncstatus) with its "
            "`sync_id`.",
            "A `dry_run` finishes before answering, whatever `async` says, and answers `200` "
            "with what a sync would update.",
            "While a sync is running, a new request answers `200` with that sync's status "
            "instead of starting another.",
            "See Model Synchronization & Auto-Updates in "
            "[Server Configuration](../guide/configuration/README.md).",
        };
        s.args = {
            {"models", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}}, false, Support::Available,
             "Models to sync. Defaults to every downloaded model."},
            {"dry_run", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Check for updates without downloading them. Defaults to `false`."},
            {"async", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Run in the background and answer immediately. Defaults to `true`."},
            {"attach_if_running", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` reports a running sync, or with `async: false` waits for it, without adding "
             "this request's models to it. Defaults to `false`, which queues the models into the "
             "running sync."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_sync_status_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"models", ordered_json::array({"Qwen3-0.6B-GGUF"})}, {"dry_run", true}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgInvalid, kOffline, kDryRunFailed, kSyncFailed};
        s.request_format = RequestFormat::OptionalJson;
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (ctx_.config->offline()) {
            write_plain_error(res, kOffline, "Cannot sync model updates while offline=true");
            return;
        }

        std::vector<std::string> target_models;
        bool dry_run = false;
        bool async_dispatch = true;
        bool async_specified = false;
        bool attach_if_running = false;

        const json& body = req.body;
        if (body.contains("models")) {
            target_models = body["models"].get<std::vector<std::string>>();
        }
        if (body.contains("dry_run")) {
            dry_run = body["dry_run"].get<bool>();
        }
        if (body.contains("async")) {
            async_dispatch = body["async"].get<bool>();
            async_specified = true;
        }
        if (body.contains("attach_if_running")) {
            attach_if_running = body["attach_if_running"].get<bool>();
        }

        ModelManager* model_manager = ctx_.model_manager;

        if (dry_run) {
            try {
                json sync_result = model_manager->sync_models(target_models, true);
                res.set_content(sync_result.dump(), "application/json");
                res.status = 200;
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Model sync dry-run failed: " << e.what() << std::endl;
                write_plain_error(res, kDryRunFailed, std::string("Model sync dry-run failed: ") + e.what());
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

            ctx_.server->start_background_sync();

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
            write_plain_error(res, kSyncFailed, std::string("Model sync failed: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_sync_route(ServerContext& ctx) {
    return std::make_unique<ModelsSyncRoute>(ctx);
}

} // namespace lemon
