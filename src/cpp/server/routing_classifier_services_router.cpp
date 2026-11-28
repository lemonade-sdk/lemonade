#include "lemon/routing_classifier_services.h"

#include "lemon/router.h"

#include <lemon/models/model_info.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
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

CostServices make_cached_cost_services(
    std::function<std::uint64_t()> generation,
    std::function<std::optional<CostInfo>(const std::string& candidate)> resolve) {
    // Shared by every routed request: make_router_cost_services() runs per request.
    // The bound holds even if a registry ever outgrows it.
    static constexpr std::size_t kMaxCachedCosts = 1024;
    static std::mutex cache_mu;
    static std::map<std::string, CostInfo> cache;
    static std::uint64_t cache_generation = 0;

    CostServices services;
    services.cost_of = [generation = std::move(generation),
                        resolve = std::move(resolve)](const std::string& candidate) -> CostInfo {
        const std::uint64_t current = generation();
        {
            std::lock_guard<std::mutex> lock(cache_mu);
            if (current > cache_generation) {
                cache.clear();
                cache_generation = current;
            }
            auto it = cache.find(candidate);
            if (cache_generation == current && it != cache.end()) {
                return it->second;
            }
        }

        std::optional<CostInfo> info = resolve(candidate);
        if (!info) {
            return CostInfo{};
        }

        std::lock_guard<std::mutex> lock(cache_mu);
        if (cache_generation == current) {
            if (cache.size() >= kMaxCachedCosts) {
                cache.clear();
            }
            cache.emplace(candidate, *info);
        }
        return *info;
    };
    return services;
}

CostServices make_router_cost_services(Router& router) {
    return make_cached_cost_services(
        [&router] { return router.models_generation(); },
        [&router](const std::string& candidate) -> std::optional<CostInfo> {
            std::optional<ModelInfo> model = router.try_get_model_info(candidate);
            if (!model) {
                return std::nullopt;
            }
            return resolve_model_cost_info(*model);
        });
}

} // namespace lemon
