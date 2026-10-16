#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_sync.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;


class CheckUpdatesRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.models_check_updates";
        s.methods = {"POST"};
        s.paths = {"models/check-updates"};
        s.summary = "Manually check downloaded models for upstream updates";
        s.description =
            "Checks downloaded models for newer commits in their registry. It is the manual "
            "counterpart to the startup update check and runs even when "
            "`auto_check_model_updates` is `false`.";
        s.notes = {
            "The `lemonade check-updates` command uses this endpoint.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "updates_available", "models", "failed_models"],
            "properties": {
                "status": {"enum": ["success", "failed"], "description": "`failed` when any model's check failed."},
                "updates_available": {"type": "integer", "description": "How many models have an update."},
                "models": {"type": "array", "items": {"type": "string"}, "description": "Models with a newer upstream commit."},
                "failed_models": {"type": "object", "additionalProperties": {"type": "string"},
                                  "description": "Each model whose check failed, with its error."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kOfflineUpdates, kUnexpectedException};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        // A manual check is intentionally independent from
        // auto_check_model_updates, but full offline mode remains authoritative.
        if (refuse_offline_updates(ctx_, res, "check")) {
            return;
        }

        try {
            auto check_res = ctx_.model_manager->check_for_model_updates();

            json response = {
                {"status", check_res.failed_models.empty() ? "success" : "failed"},
                {"updates_available", check_res.updated_models.size()},
                {"models", check_res.updated_models},
                {"failed_models", check_res.failed_models}
            };

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Manual model update check failed: " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, std::string("Model update check failed: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_check_updates_route(ServerContext& ctx) {
    return std::make_unique<CheckUpdatesRoute>(ctx);
}

} // namespace lemon
