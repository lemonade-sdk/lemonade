#pragma once

#include "lemon/model_types.h"
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

// Looks up a resolved component's deployment ModelType. Server integration
// binds this to the model registry (ModelInfo::type); pure parser tests leave
// it unset, which skips the capability check entirely (matching prior
// behavior — this option is additive and opt-in).
//
// Returns nullopt when the type genuinely cannot be established (e.g. an
// inline collection component with no matching `models[]` definition to fall
// back on) — the parser treats that as a hard error rather than guessing a
// type, since guessing wrong is worse than not checking at all: it can both
// reject valid configs and silently accept invalid ones.
using RoutingModelTypeResolver =
    std::function<std::optional<ModelType>(const std::string& resolved_model)>;

struct RoutingPolicyParseOptions {
    RoutingComponentResolver resolve_component;
    RoutingModelTypeResolver get_model_type;
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
