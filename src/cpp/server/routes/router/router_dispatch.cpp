#include "lemon/server/router_dispatch.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/server_context.h"
#include "lemon/server/shared_args.h"
#include "lemon/utils/conversation_fingerprint.h"

namespace lemon {

using json = nlohmann::json;

std::optional<RouterDispatchResult> router_dispatch(ServerContext& ctx, std::string& model,
                                                    bool want_trace, json& body) {
    if (model.empty()) {
        return std::nullopt;
    }
    const std::string requested_model = model;
    try {
        if (!ctx.model_manager->model_exists(requested_model)) {
            return std::nullopt;
        }
        ModelInfo info = ctx.model_manager->get_model_info(requested_model);
        if (!is_router_collection_recipe(info.recipe)) {
            return std::nullopt;
        }
        // The policy is parsed once when the models cache is built. A missing one means
        // the collection failed to parse, so the request keeps its model (fail open).
        if (!info.route_policy) {
            LOG(WARNING, "Server") << "Router collection '" << info.model_name
                                   << "' has no parsed routing policy" << std::endl;
            return std::nullopt;
        }

        // The engine owns its policy and is rebuilt per request, because its
        // classifier services are bound to the live Router.
        RoutePolicy policy = *info.route_policy;
        ModelLoader* loader = ctx.model_loader;
        ClassifierServices services = make_router_classifier_services(
            *ctx.router, [loader](const std::string& m) {
                loader->ensure_loaded(m, json::object(), LoadPurpose::RoutingDependency);
            });
        CostServices cost_services = make_router_cost_services(*ctx.router);
        RoutingPolicyEngine engine(std::move(policy), std::move(services),
                                   std::move(cost_services));

        RouteContext route_context = build_route_context(body, info.model_name);
        Decision decision = engine.route(route_context, want_trace);

        RouterDispatchResult result;
        result.requested_model = requested_model;
        result.selected_model = decision.route_to;
        result.decision = std::move(decision);
        ctx.router->note_route_decision(utils::conversation_fingerprint(body),
                                        result.selected_model);

        LOG(INFO, "Server") << "Router collection '" << requested_model << "' -> '"
                            << result.selected_model << "'" << std::endl;
        body[model_loading::kModel.name] = result.selected_model;
        body.erase(openai::text::kRouteTrace.name);
        model = result.selected_model;
        return result;
    } catch (const RouterResidencyConflictException&) {
        // A hardware-policy conflict is not a routing miss: answering it with 409
        // beats falling back to loading the collection recipe itself.
        throw;
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Router collection dispatch failed for '"
                               << requested_model << "': " << e.what() << std::endl;
    }
    return std::nullopt;
}

std::set<std::string> active_policy_helper_models(ModelManager& model_manager) {
    std::set<std::string> needed;
    for (const auto& [name, info] : model_manager.get_supported_models()) {
        (void)name;
        if (!info.route_policy) {
            continue;
        }
        for (const auto& helper : info.route_policy->helper_models) {
            needed.insert(helper);
        }
    }
    return needed;
}

} // namespace lemon
