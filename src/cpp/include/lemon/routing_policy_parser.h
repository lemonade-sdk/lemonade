#pragma once

#include "lemon/routing_policy.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lemon {

// Resolves a component name from collection JSON into the name the engine should
// route to. Server integration can bind this to ModelManager::resolve_model_name;
// pure parser tests use the identity resolver.
using RoutingComponentResolver =
    std::function<std::optional<std::string>(const std::string& component)>;

struct RoutingPolicyParseOptions {
    RoutingComponentResolver resolve_component;
    bool require_declared_components = true;
};

// Parser key registries. The parser rejects any key outside these sets; the
// schema-parity test compares them to route_policy.schema.json so parser and
// schema vocabulary cannot drift silently.
const std::set<std::string>& routing_policy_root_keys();
const std::set<std::string>& routing_block_keys();
const std::set<std::string>& routing_router_keys();
const std::set<std::string>& routing_classifier_keys();
const std::set<std::string>& routing_rule_keys();
const std::set<std::string>& routing_match_expr_keys();
const std::set<std::string>& routing_metadata_match_keys();

// Parse a full collection.router document into engine-ready policy state.
// Throws std::invalid_argument with a user-facing message on validation errors.
RoutePolicy parse_route_policy_collection(
    const json& collection_json,
    const RoutingPolicyParseOptions& options = RoutingPolicyParseOptions{});

} // namespace lemon
