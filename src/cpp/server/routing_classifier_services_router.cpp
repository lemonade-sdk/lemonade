#include "lemon/routing_classifier_services.h"

#include "lemon/router.h"

#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace lemon {
namespace {

// A caller can name arbitrary candidate strings (e.g. /v1/routing/validate's
// identity resolver), so without a cap, requests naming a steady stream of
// unique names would grow make_router_cost_services' cache without limit.
// File-scope rather than a lambda-local constexpr: MSVC requires an explicit
// capture for the latter (error C3493), unlike GCC/Clang.
constexpr std::size_t kMaxCachedCandidates = 4096;

struct CostCache {
    std::mutex mu;
    std::map<std::string, CostInfo> entries;
    uint64_t generation = 0;
};

} // namespace

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
    // Memo keyed by candidate name, valid for one registry-change generation:
    // avoids a registry/build_cache hit on every routed request while still
    // picking up a price the moment it changes (model add/edit/remove, cloud
    // discovery, on-disk edit) instead of only on restart. Bounded (see
    // kMaxCachedCandidates above); past the cap, a new name just isn't
    // cached , it costs a repeat lookup on every use rather than evicting an
    // already-cached real model.
    // Owned by the returned services rather than file-static: two Routers can
    // price the same candidate differently, and a shared memo would let one
    // answer for the other whenever their generations happened to match.
    auto state = std::make_shared<CostCache>();

    CostServices services;
    services.cost_of = [&router, state](const std::string& candidate) -> CostInfo {
        const uint64_t generation = router.registry_generation();
        {
            std::lock_guard<std::mutex> lock(state->mu);
            // Strictly greater, never just different: the read above is
            // unlocked, so a thread that stalls here can arrive carrying an
            // older generation than the one already published. Treating that as
            // a change would clear a fresher cache and move the generation back.
            if (generation > state->generation) {
                state->entries.clear();
                state->generation = generation;
            }
            auto it = state->entries.find(candidate);
            if (it != state->entries.end()) {
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

        std::lock_guard<std::mutex> lock(state->mu);
        // Publish only under the generation this price was read for.
        if (generation != state->generation
            || state->entries.size() >= kMaxCachedCandidates) {
            return info;
        }
        auto [it, inserted] = state->entries.emplace(candidate, info);
        (void)inserted;
        return it->second;
    };
    return services;
}

} // namespace lemon
