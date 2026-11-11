#include "lemon/routing_classifier_services.h"

#include "lemon/router.h"

#include <lemon/models/model_info.h>
#include <cstdint>
#include <map>
#include <mutex>
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
        [&router](const std::string& model) { return router.get_model_type(model); },
        [&router](const nlohmann::ordered_json& request) {
            return json(router.systemone(request));
        });
}

CostServices make_router_cost_services(Router& router) {
    // Shared by every routed request. Stamped with the registry generation, so a
    // price changed through /pull is seen on the next request instead of after
    // a restart.
    static std::mutex cache_mu;
    static std::map<std::string, CostInfo> cache;
    static std::uint64_t cache_generation = 0;

    CostServices services;
    services.cost_of = [&router](const std::string& candidate) -> CostInfo {
        const std::uint64_t generation = router.models_generation();
        {
            std::lock_guard<std::mutex> lock(cache_mu);
            if (generation > cache_generation) {
                cache.clear();
                cache_generation = generation;
            }
            auto it = cache.find(candidate);
            if (cache_generation == generation && it != cache.end()) {
                return it->second;
            }
        }

        CostInfo info;
        if (std::optional<ModelInfo> model = router.try_get_model_info(candidate)) {
            info = resolve_model_cost_info(*model);
        }

        std::lock_guard<std::mutex> lock(cache_mu);
        if (cache_generation == generation) {
            cache.emplace(candidate, info);
        }
        return info;
    };
    return services;
}

} // namespace lemon
