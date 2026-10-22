#include <memory>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_sync.h"
#include "lemon/server/shared_args.h"

namespace lemon {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {


const RouteArg kModels{"models", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}}, false, Support::Available,
                       "Models to sync. Defaults to every downloaded model."};
const RouteArg kAsync{"async", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                      "Run in the background and answer immediately. Defaults to `true`."};
const RouteArg kAttachIfRunning{"attach_if_running", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                                "`true` reports a running sync, or with `async: false` waits for it, without adding "
                                "this request's models to it. Defaults to `false`, which queues the models into the "
                                "running sync."};

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
        s.args = {kModels, common::kDryRun, kAsync, kAttachIfRunning};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_sync_status_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"models", ordered_json::array({"Qwen3-0.6B-GGUF"})}, {"dry_run", true}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgInvalid, kOfflineUpdates, kUnexpectedException};
        s.request_format = RequestFormat::OptionalJson;
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (refuse_offline_updates(ctx_, res, "sync")) {
            return;
        }

        std::vector<std::string> target_models;
        bool dry_run = false;
        bool async_dispatch = true;
        bool async_specified = false;
        bool attach_if_running = false;

        if (req.has(kModels)) {
            target_models = req.arg(kModels).get<std::vector<std::string>>();
        }
        if (req.has(common::kDryRun)) {
            dry_run = req.arg(common::kDryRun).get<bool>();
        }
        if (req.has(kAsync)) {
            async_dispatch = req.arg(kAsync).get<bool>();
            async_specified = true;
        }
        if (req.has(kAttachIfRunning)) {
            attach_if_running = req.arg(kAttachIfRunning).get<bool>();
        }

        ModelManager* model_manager = ctx_.model_manager;

        if (dry_run) {
            try {
                json sync_result = model_manager->sync_models(target_models, true);
                res.set_content(sync_result.dump(), "application/json");
                res.status = 200;
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Model sync dry-run failed: " << e.what() << std::endl;
                write_plain_error(res, kUnexpectedException, std::string("Model sync dry-run failed: ") + e.what());
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
            write_plain_error(res, kUnexpectedException, std::string("Model sync failed: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_sync_route(ServerContext& ctx) {
    return std::make_unique<ModelsSyncRoute>(ctx);
}

} // namespace lemon
