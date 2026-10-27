#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_sync.h"

namespace lemon {

using json = nlohmann::json;

namespace {

const RouteError kInvalidSyncId{
    400, "", "`sync_id` is too large for 64 bits"};

const RouteArg kSyncId{"sync_id", ArgIn::Query, {{"type", "integer"}}, false, Support::Available,
                       "Sync to report. Defaults to the latest. For a sync that has finished, the answer is "
                       "the status recorded when it finished, `completed_sync_id` and `already_in_progress` "
                       "included."};

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
            "[`POST /internal/models/sync`](#post-internalmodelssync).";
        s.notes = {
            "A `sync_id` that is neither running nor recorded answers `200` with "
            "`status: \"not_found\"`.",
        };
        s.args = {kSyncId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_sync_status_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kInvalidSyncId, kUnexpectedException};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            uint64_t sync_id = 0;
            if (req.has(kSyncId)) {
                const json& value = req.arg(kSyncId);
                try {
                    sync_id = value.is_number_integer() ? value.get<uint64_t>()
                                                        : std::stoull(value.get<std::string>());
                } catch (...) {
                    write_plain_error(res, kInvalidSyncId, "Invalid sync_id parameter");
                    return;
                }
            }
            json status = ctx_.model_manager->get_sync_status(sync_id);
            res.set_content(status.dump(), "application/json");
            res.status = 200;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to get sync status: " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, std::string("Failed to get sync status: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_sync_status_route(ServerContext& ctx) {
    return std::make_unique<ModelsSyncStatusRoute>(ctx);
}

} // namespace lemon
