#include <lemon/models/routing_parse_options.h>

#include <lemon/backends/backend_descriptor_registry.h>

#include <optional>

namespace lemon {
namespace models {

// `systemone` is a capability label, legal on any recipe, so the label alone
// does not make an ONNX classifier answer /v1/systemone.
RoutingModelFacts routing_model_facts(ModelType type, const std::vector<std::string>& labels,
                                      const std::string& recipe) {
    RoutingModelFacts facts;
    facts.type = type;
    facts.serves_systemone = type == ModelType::CLASSIFICATION &&
                             has_label(labels, "systemone") &&
                             backends::recipe_gates_mode_on(recipe, "systemone");
    // A decision model turns per-request labels into one choice question.
    facts.serves_zero_shot = facts.serves_systemone ||
                             (type == ModelType::CLASSIFICATION && has_label(labels, "zero-shot"));
    return facts;
}

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
    auto lookup = [tiers](const std::string& name) -> std::optional<RoutingModelFacts> {
        if (tiers.facts) {
            if (auto facts = tiers.facts(name)) return facts;
        }
        return tiers.facts_fallback ? tiers.facts_fallback(name) : std::nullopt;
    };
    options.get_model_type = [lookup](const std::string& name) -> std::optional<ModelType> {
        const std::optional<RoutingModelFacts> facts = lookup(name);
        if (!facts) return std::nullopt;
        return facts->type;
    };
    options.serves_zero_shot = [lookup](const std::string& name) {
        const std::optional<RoutingModelFacts> facts = lookup(name);
        return facts && facts->serves_zero_shot;
    };
    options.serves_systemone = [lookup](const std::string& name) {
        const std::optional<RoutingModelFacts> facts = lookup(name);
        return facts && facts->serves_systemone;
    };
    return options;
}

}  // namespace models
}  // namespace lemon
