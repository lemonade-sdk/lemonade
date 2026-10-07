#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {

using json = nlohmann::json;

namespace {

class ModelsSyncStatusRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.models_sync_status";
        s.methods = {"GET"};
        s.paths = {"models/sync/status"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Read the progress of a model sync";
        s.description =
            "Returns the status of a model sync started by "
            "[`POST /internal/models/sync`](#post-internalmodelssync), including download "
            "progress while it runs.";
        s.notes = {
            "A `sync_id` that is neither running nor recorded answers `200` with "
            "`status: \"not_found\"`.",
        };
        s.args = {
            {"sync_id", ArgIn::Query, {{"type", "integer"}}, false, Support::Available,
             "Sync to report. Defaults to the latest."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ModelManager::sync_status_schema();
        response.example = json::object();
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            uint64_t sync_id = 0;
            if (req.http.has_param("sync_id")) {
                try {
                    sync_id = std::stoull(req.http.get_param_value("sync_id"));
                } catch (...) {
                    write_plain_error(res, 400, "Invalid sync_id parameter");
                    return;
                }
            }
            json status = ctx_.model_manager->get_sync_status(sync_id);
            res.set_content(status.dump(), "application/json");
            res.status = 200;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to get sync status: " << e.what() << std::endl;
            write_plain_error(res, 500, std::string("Failed to get sync status: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_sync_status_route(ServerContext& ctx) {
    return std::make_unique<ModelsSyncStatusRoute>(ctx);
}

} // namespace lemon
