#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Checks downloaded Hugging Face-backed models for newer upstream commits. It is the "
            "manual counterpart to the startup update check and works even when "
            "`auto_check_model_updates=false`.";
        s.notes = {
            "Offline mode remains authoritative: with `offline=true` this answers `409` and makes "
            "no network requests.",
            "The CLI runs the same check with `lemonade check-updates`.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status", "updates_available", "models", "failed_models"],
            "properties": {
                "status": {"enum": ["success", "failed"], "description": "failed when any model's check failed."},
                "updates_available": {"type": "integer"},
                "models": {"type": "array", "items": {"type": "string"}, "description": "Models with a newer upstream commit."},
                "failed_models": {"type": "object", "additionalProperties": {"type": "string"},
                                  "description": "Each model whose check failed, with its error."}
            }
        })");
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        // A manual check is intentionally independent from
        // auto_check_model_updates, but full offline mode remains authoritative.
        if (ctx_.config->offline()) {
            write_plain_error(res, 409, "Cannot check model updates while offline=true");
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
            write_plain_error(res, 500, std::string("Model update check failed: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_check_updates_route(ServerContext& ctx) {
    return std::make_unique<CheckUpdatesRoute>(ctx);
}

} // namespace lemon
