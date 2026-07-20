#include "lemon/routing_classifier_services.h"

#include "lemon/router.h"

#include <utility>

namespace lemon {

ClassifierServices make_router_classifier_services(
    Router& router,
    EnsureClassifierModelLoaded ensure_loaded) {
    return make_classifier_services_from_router_calls(
        [&router](const json& request) { return router.embeddings(request); },
        [&router](const json& request) { return router.chat_completion(request); },
        std::move(ensure_loaded),
        [&router](const json& request) { return router.classify(request); },
        [&router](const std::string& model) { return router.get_model_type(model); });
}

CostServices make_router_cost_services(Router& router) {
    CostServices services;
    services.cost_of = [&router](const std::string& candidate) -> CostInfo {
        std::optional<ModelInfo> info = router.try_get_model_info(candidate);
        if (!info) {
            return CostInfo{};
        }
        return resolve_cost_info(info->cost_input_per_million,
                                 info->cost_output_per_million,
                                 info->extras);
    };
    return services;
}

} // namespace lemon
