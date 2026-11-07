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

// The one place that answers "does this model name a router collection?".
// Dispatch and the responses-chain guard must agree on that, so neither derives
// it on its own. Registry exceptions propagate: a failed lookup means something
// different at each call site.
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

// Router::reconcile_routing_helpers() needs the routing helpers that every active
// collection.router policy keeps resident, so it can reclaim the rest.
std::set<std::string> active_policy_helper_models(ModelManager& model_manager);

} // namespace lemon
