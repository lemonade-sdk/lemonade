#pragma once

#include <lemon/model_types.h>
#include <lemon/routing_policy_parser.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// What routing-policy registration needs to know about one component.
struct RoutingModelFacts {
    ModelType type = ModelType::LLM;
    bool serves_zero_shot = false;
    bool serves_systemone = false;
    // Set only when a policy is registered: a stored policy that fails to parse
    // loses all its routing, so build_cache() leaves the refusal to each request.
    std::string load_refusal;
};

RoutingModelFacts routing_model_facts(ModelType type, const std::vector<std::string>& labels,
                                      const std::string& recipe);

// Every routing-policy parse site resolves a component the same way: alias first,
// falling back to the name as authored; then a primary facts lookup with one
// fallback tier behind it. #2748 was that shape drifting between two hand-written
// copies, so both are built from here. Only the tiers differ -- which model set
// each site consults, and what the fallback is.
struct RoutingResolutionTiers {
    std::function<std::optional<std::string>(const std::string&)> alias;
    std::function<std::optional<RoutingModelFacts>(const std::string&)> facts;
    std::function<std::optional<RoutingModelFacts>(const std::string&)> facts_fallback;
};

RoutingPolicyParseOptions make_routing_parse_options(RoutingResolutionTiers tiers);

}  // namespace models
}  // namespace lemon
