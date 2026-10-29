#pragma once

#include <lemon/model_types.h>
#include <lemon/routing_policy_parser.h>

#include <functional>
#include <optional>
#include <string>

namespace lemon {
namespace models {

// Every routing-policy parse site resolves a component the same way: alias first,
// falling back to the name as authored; then a primary type lookup with one
// fallback tier behind it. #2748 was that shape drifting between two hand-written
// copies, so both are built from here. Only the tiers differ -- which model set
// each site consults, and what the fallback is.
struct RoutingResolutionTiers {
    std::function<std::optional<std::string>(const std::string&)> alias;
    std::function<std::optional<ModelType>(const std::string&)> type;
    std::function<std::optional<ModelType>(const std::string&)> type_fallback;
};

RoutingPolicyParseOptions make_routing_parse_options(RoutingResolutionTiers tiers);

}  // namespace models
}  // namespace lemon
