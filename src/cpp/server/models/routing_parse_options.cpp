#include <lemon/models/routing_parse_options.h>

#include <optional>

namespace lemon {
namespace models {

RoutingPolicyParseOptions make_routing_parse_options(RoutingResolutionTiers tiers) {
    RoutingPolicyParseOptions options;
    options.resolve_component =
        [tiers](const std::string& name) -> std::optional<std::string> {
        if (tiers.alias) {
            if (auto resolved = tiers.alias(name)) return *resolved;
        }
        // An unresolvable name stays as authored so component-role validation
        // can still report it by the name the author actually wrote.
        return name;
    };
    options.get_model_type = [tiers](const std::string& name) -> std::optional<ModelType> {
        if (tiers.type) {
            if (auto type = tiers.type(name)) return type;
        }
        return tiers.type_fallback ? tiers.type_fallback(name) : std::nullopt;
    };
    return options;
}

}  // namespace models
}  // namespace lemon
