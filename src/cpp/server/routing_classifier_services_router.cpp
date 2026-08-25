#include "lemon/routing_classifier_services.h"

#include "lemon/model_manager.h"
#include "lemon/router.h"

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
        [&router](const std::string& model) { return router.get_model_type(model); });
}

CostServices make_router_cost_services(Router& router, ModelManager& model_manager) {
    // Memo keyed by candidate name, valid for one registry-change generation:
    // avoids a registry/build_cache hit on every routed request while still
    // picking up a price the moment it changes (model add/edit/remove, cloud
    // discovery, on-disk edit) instead of only on restart. Bounded: a caller
    // can name arbitrary candidate strings (e.g. /v1/routing/validate's
    // identity resolver), so without a cap, requests naming a steady stream
    // of unique names would grow this process-global map without limit.
    // Past the cap, a new name just isn't cached — it costs a repeat lookup
    // on every use rather than evicting an already-cached real model.
    constexpr std::size_t kMaxCachedCandidates = 4096;
    static std::mutex cache_mu;
    static std::map<std::string, CostInfo> cache;
    static uint64_t cached_generation = 0;

    CostServices services;
    services.cost_of = [&router, &model_manager](const std::string& candidate) -> CostInfo {
        const uint64_t generation = model_manager.current_notify_generation();
        {
            std::lock_guard<std::mutex> lock(cache_mu);
            if (generation != cached_generation) {
                cache.clear();
                cached_generation = generation;
            }
            auto it = cache.find(candidate);
            if (it != cache.end()) {
                return it->second;
            }
        }

        CostInfo info;
        std::optional<ModelInfo> model = router.try_get_model_info(candidate);
        if (model) {
            const std::optional<double> typed_input =
                model->cost_input_per_million >= 0.0
                    ? std::optional<double>{model->cost_input_per_million}
                    : std::nullopt;
            const std::optional<double> typed_output =
                model->cost_output_per_million >= 0.0
                    ? std::optional<double>{model->cost_output_per_million}
                    : std::nullopt;
            info = resolve_cost_info(typed_input, typed_output, model->extras);
        }

        std::lock_guard<std::mutex> lock(cache_mu);
        if (cache.size() >= kMaxCachedCandidates) {
            return info;
        }
        auto [it, inserted] = cache.emplace(candidate, info);
        (void)inserted;
        return it->second;
    };
    return services;
}

} // namespace lemon
