#pragma once

#include <optional>
#include <set>
#include <string>

#include "lemon/routing_policy.h"

namespace lemon {

class ModelManager;
struct RouteRequest;
struct ServerContext;

struct RouterDispatchResult {
    std::string requested_model;
    std::string selected_model;
    Decision decision;
};

// Naming a collection.router model makes a completion request route itself: this
// runs the collection's policy, rewrites req.body["model"] and req.model to the
// selected candidate, and returns the decision. Returns std::nullopt, leaving the
// request untouched, for any other model or when routing fails. A residency
// conflict is the one failure it rethrows, since falling back would load the
// collection itself.
std::optional<RouterDispatchResult> router_dispatch(ServerContext& ctx, RouteRequest& req);

// Router::reconcile_routing_helpers() needs the routing helpers that every active
// collection.router policy keeps resident, so it can reclaim the rest.
std::set<std::string> active_policy_helper_models(ModelManager& model_manager);

} // namespace lemon
