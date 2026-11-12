#include <lemon/utils/aixlog.hpp>

#include "lemon/canonical_id.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidRegistration{400, "", "Only an existing user.* model definition can be unregistered"};
const RouteError kRegistrationBusy{409, "", "Unload the model before removing its registration"};

class UnregisterRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.models_unregister";
        s.methods = {"POST"};
        s.paths = {"models/unregister"};
        s.summary = "Unregister a custom model without deleting its files";
        s.description =
            "Removes an existing `user.*` registration and its saved inference-engine "
            "settings. Downloaded checkpoints are left untouched; an overridden "
            "built-in becomes the default for its bare model name again.";
        s.args = {model_management::kModelName};
        s.request_format = RequestFormat::Json;

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "model_name"],
            "properties": {
                "status": {"const": "success", "description": "Always `success`."},
                "model_name": {"type": "string", "description": "Canonical `user.*` model name that was unregistered."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.models_register", ""}};
        example.args = {{"model_name", "$lemonade.models_register/canonical_model_name"}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kInvalidRegistration, kRegistrationBusy,
                    kUnexpectedException};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const std::string id = req.arg(model_management::kModelName).get<std::string>();
        if (!is_user_model_name(id) || is_reserved_registration_name(id) ||
            !ctx_.model_manager->model_exists(id)) {
            write_plain_error(res, kInvalidRegistration, "Not an existing user model: " + id);
            return;
        }

        // A loaded or in-flight backend must not outlive its registration.
        // The reservation also blocks loads that already resolved this id but
        // have not yet acquired the router load slot.
        if (!ctx_.router->begin_model_unregistration(id)) {
            write_plain_error(res, kRegistrationBusy,
                              "Unload the model or finish the ongoing load before restoring it");
            return;
        }
        struct Reservation {
            Router* router;
            std::string id;
            ~Reservation() { router->end_model_unregistration(id); }
        } reservation{ctx_.router, id};

        try {
            ctx_.model_manager->unregister_user_model(id);
            res.set_content(json{{"status", "success"}, {"model_name", id}}.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Cannot unregister " << id << ": " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_unregister_route(ServerContext& ctx) {
    return std::make_unique<UnregisterRoute>(ctx);
}

} // namespace lemon
