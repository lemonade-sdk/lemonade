#pragma once

#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/routing_policy.h"

namespace lemon {

class ModelManager;
struct ModelInfo;
struct ServerContext;

// The collection.router model that `model` names, or std::nullopt for any other
// model. Registry exceptions propagate.
std::optional<ModelInfo> router_collection_info(ServerContext& ctx, const std::string& model);

struct RouterDispatchResult {
    std::string requested_model;
    std::string selected_model;
    Decision decision;
};

// Naming a collection.router model makes a completion request route itself: this
// runs the collection's policy on body, rewrites model and the body's model to the
// selected candidate, and returns the decision. Returns std::nullopt, leaving both
// untouched, for any other model or when routing fails. A residency conflict is the
// one failure it rethrows, since falling back would load the collection itself.
std::optional<RouterDispatchResult> router_dispatch(ServerContext& ctx, std::string& model,
                                                    bool want_trace, nlohmann::json& body);

// The same, for a collection already resolved with router_collection_info(), so a
// check made on it and the routing read one snapshot of the registry.
std::optional<RouterDispatchResult> router_dispatch(ServerContext& ctx, std::string& model,
                                                    bool want_trace, nlohmann::json& body,
                                                    const std::optional<ModelInfo>& collection);

// Router::reconcile_routing_helpers() needs the routing helpers that every active
// collection.router policy keeps resident, so it can reclaim the rest.
std::set<std::string> active_policy_helper_models(ModelManager& model_manager);

} // namespace lemon
