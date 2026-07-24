#include "lemon/routing_policy_parser.h"

#include "lemon/model_types.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace lemon {
namespace {

std::string path_index(const std::string& path, std::size_t i) {
    return path + "[" + std::to_string(i) + "]";
}

void require_object(const json& value, const std::string& path) {
    if (!value.is_object()) {
        throw std::invalid_argument(path + " must be an object");
    }
}

void reject_unknown_keys(const json& value,
                         const std::set<std::string>& accepted,
                         const std::string& path) {
    require_object(value, path);
    for (const auto& [key, _] : value.items()) {
        if (accepted.count(key) == 0) {
            throw std::invalid_argument(path + " contains unknown key '" + key + "'");
        }
    }
}

const json& required_field(const json& value,
                           const std::string& key,
                           const std::string& path) {
    if (!value.contains(key)) {
        throw std::invalid_argument(path + " is missing required key '" + key + "'");
    }
    return value.at(key);
}

std::string required_string(const json& value,
                            const std::string& key,
                            const std::string& path) {
    const json& field = required_field(value, key, path);
    if (!field.is_string() || field.get<std::string>().empty()) {
        throw std::invalid_argument(path + "." + key + " must be a non-empty string");
    }
    return field.get<std::string>();
}

std::string optional_string(const json& value,
                            const std::string& key,
                            const std::string& path) {
    if (!value.contains(key)) {
        return "";
    }
    if (!value.at(key).is_string() || value.at(key).get<std::string>().empty()) {
        throw std::invalid_argument(path + "." + key + " must be a non-empty string");
    }
    return value.at(key).get<std::string>();
}

bool is_safe_rule_id(const std::string& id) {
    for (char ch : id) {
        const bool ok = (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string required_rule_id(const json& value,
                             const std::string& key,
                             const std::string& path) {
    std::string id = required_string(value, key, path);
    if (!is_safe_rule_id(id)) {
        throw std::invalid_argument(
            path + "." + key + " must match [A-Za-z0-9._-]");
    }
    return id;
}

std::string schema_major(const std::string& version) {
    const auto dot = version.find('.');
    return dot == std::string::npos ? version : version.substr(0, dot);
}

void validate_version_1(const json& collection_json) {
    const std::string version = required_string(collection_json, "version", "collection");
    const std::string major = schema_major(version);
    if (major != "1") {
        throw std::invalid_argument(
            "Unsupported collection.router schema major '" + major +
            "' from version '" + version + "'; supported major is '1'");
    }
    if (version != "1") {
        throw std::invalid_argument(
            "Unsupported collection.router schema version '" + version +
            "'; this parser implements version '1'");
    }
}

std::string resolve_component(const std::string& component,
                              const RoutingPolicyParseOptions& options,
                              const std::string& path) {
    std::optional<std::string> resolved =
        options.resolve_component ? options.resolve_component(component)
                                  : std::optional<std::string>(component);
    if (!resolved.has_value() || resolved->empty()) {
        throw std::invalid_argument(path + " references unknown component '" + component + "'");
    }
    return *resolved;
}

std::set<std::string> parse_declared_components(const json& collection_json,
                                                const RoutingPolicyParseOptions& options) {
    std::set<std::string> components;
    if (!collection_json.contains("components")) {
        if (options.require_declared_components) {
            throw std::invalid_argument("collection.router requires a non-empty components array");
        }
        return components;
    }
    const json& value = collection_json.at("components");
    if (!value.is_array() || value.empty()) {
        if (options.require_declared_components) {
            throw std::invalid_argument("collection.components must be a non-empty array");
        }
        return components;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_string() || value[i].get<std::string>().empty()) {
            throw std::invalid_argument(path_index("collection.components", i) +
                                        " must be a non-empty string");
        }
        std::string resolved = resolve_component(value[i].get<std::string>(), options,
                                                 path_index("collection.components", i));
        components.insert(std::move(resolved));
    }
    return components;
}

void require_declared(const std::string& resolved,
                      const std::string& original,
                      const std::set<std::string>& declared,
                      const RoutingPolicyParseOptions& options,
                      const std::string& path) {
    if (!options.require_declared_components) {
        return;
    }
    if (declared.count(resolved) == 0) {
        throw std::invalid_argument(path + " references component '" + original +
                                    "' that is not declared in collection.components");
    }
}

std::vector<std::string> parse_candidates(const json& routing,
                                          const std::set<std::string>& declared,
                                          const RoutingPolicyParseOptions& options) {
    const json& value = required_field(routing, "candidates", "routing");
    if (!value.is_array() || value.empty()) {
        throw std::invalid_argument("routing.candidates must be a non-empty array");
    }
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::string path = path_index("routing.candidates", i);
        if (!value[i].is_string() || value[i].get<std::string>().empty()) {
            throw std::invalid_argument(path + " must be a non-empty string");
        }
        const std::string original = value[i].get<std::string>();
        std::string resolved = resolve_component(original, options, path);
        require_declared(resolved, original, declared, options, path);
        if (!seen.insert(resolved).second) {
            throw std::invalid_argument(path + " duplicates candidate '" + original + "'");
        }
        candidates.push_back(std::move(resolved));
    }
    return candidates;
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void validate_score_bound(const json& leaf,
                          const std::string& key,
                          std::optional<double>& out,
                          const std::string& path) {
    if (!leaf.contains(key)) {
        return;
    }
    if (!leaf.at(key).is_number()) {
        throw std::invalid_argument(path + "." + key + " must be numeric");
    }
    const double value = leaf.at(key).get<double>();
    if (value < 0.0 || value > 1.0) {
        throw std::invalid_argument(path + "." + key + " must be in [0, 1]");
    }
    out = value;
}

void validate_string_array(const json& value,
                           const std::string& path,
                           bool require_non_empty) {
    if (!value.is_array() || (require_non_empty && value.empty())) {
        throw std::invalid_argument(path + " must be a non-empty array");
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_string() || value[i].get<std::string>().empty()) {
            throw std::invalid_argument(path_index(path, i) +
                                        " must be a non-empty string");
        }
    }
}

void validate_metadata_match(const json& spec, const std::string& path) {
    reject_unknown_keys(spec, routing_metadata_match_keys(), path);
    required_string(spec, "key", path);
    const int comparators = static_cast<int>(spec.contains("equals")) +
                            static_cast<int>(spec.contains("any")) +
                            static_cast<int>(spec.contains("exists"));
    if (comparators != 1) {
        throw std::invalid_argument(
            path + " must contain exactly one comparator: equals, any, or exists");
    }
    if (spec.contains("equals") && !spec.at("equals").is_string()) {
        throw std::invalid_argument(path + ".equals must be a string");
    }
    if (spec.contains("any")) {
        validate_string_array(spec.at("any"), path + ".any", true);
    }
    if (spec.contains("exists") && !spec.at("exists").is_boolean()) {
        throw std::invalid_argument(path + ".exists must be a boolean");
    }
}

void validate_leaf(const json& leaf,
                   const std::map<std::string, ClassifierPtr>& classifiers,
                   const std::string& path) {
    reject_unknown_keys(leaf, routing_match_expr_keys(), path);

    std::size_t condition_count = 0;
    if (leaf.contains("classifier")) {
        ++condition_count;
        const std::string id = required_string(leaf, "classifier", path);
        auto classifier_it = classifiers.find(id);
        if (classifier_it == classifiers.end()) {
            throw std::invalid_argument(path + ".classifier references unknown classifier '" +
                                        id + "'");
        }

        const auto& classifier = classifier_it->second;
        if (leaf.contains("label")) {
            const std::string label = optional_string(leaf, "label", path);
            const auto& labels = classifier->labels();
            if (labels.empty() ||
                std::find(labels.begin(), labels.end(), label) == labels.end()) {
                throw std::invalid_argument(path + ".label references unknown label '" +
                                            label + "' on classifier '" + id + "'");
            }
        } else if (!classifier->labels().empty() &&
                   !classifier->default_label().has_value()) {
            throw std::invalid_argument(
                path + " omits label but classifier '" + id +
                "' has no default_label");
        }

        std::optional<double> min_score;
        std::optional<double> max_score;
        validate_score_bound(leaf, "min_score", min_score, path);
        validate_score_bound(leaf, "max_score", max_score, path);
        if (min_score.has_value() && max_score.has_value() && *min_score > *max_score) {
            throw std::invalid_argument(path + " has min_score greater than max_score");
        }
    } else {
        for (const char* classifier_only : {"label", "min_score", "max_score"}) {
            if (leaf.contains(classifier_only)) {
                throw std::invalid_argument(path + "." + classifier_only +
                                            " requires a classifier condition");
            }
        }
    }

    for (const char* op : {"keywords_any", "keywords_all"}) {
        if (leaf.contains(op)) {
            ++condition_count;
            validate_string_array(leaf.at(op), path + "." + op, true);
        }
    }
    if (leaf.contains("regex")) {
        ++condition_count;
        if (!leaf.at("regex").is_string() || leaf.at("regex").get<std::string>().empty()) {
            throw std::invalid_argument(path + ".regex must be a non-empty string");
        }
    }
    for (const char* op : {"min_chars", "max_chars"}) {
        if (leaf.contains(op)) {
            ++condition_count;
            if (!leaf.at(op).is_number_integer() || leaf.at(op).get<long long>() < 0) {
                throw std::invalid_argument(path + "." + op +
                                            " must be a non-negative integer");
            }
        }
    }
    for (const char* op : {"has_tools", "has_images"}) {
        if (leaf.contains(op)) {
            ++condition_count;
            if (!leaf.at(op).is_boolean()) {
                throw std::invalid_argument(path + "." + op + " must be a boolean");
            }
        }
    }
    if (leaf.contains("metadata")) {
        ++condition_count;
        validate_metadata_match(leaf.at("metadata"), path + ".metadata");
    }

    if (condition_count == 0) {
        throw std::invalid_argument(path + " must contain at least one leaf condition");
    }
}

MatchExpr parse_match_expr(const json& expr,
                           const std::map<std::string, ClassifierPtr>& classifiers,
                           const std::string& path,
                           std::size_t depth) {
    if (depth > kMaxMatchExprDepth) {
        throw std::invalid_argument(path + " exceeds maximum match expression depth");
    }
    reject_unknown_keys(expr, routing_match_expr_keys(), path);

    const bool has_any = expr.contains("any");
    const bool has_all = expr.contains("all");
    const bool has_not = expr.contains("not");
    const int logical_count = static_cast<int>(has_any) +
                              static_cast<int>(has_all) +
                              static_cast<int>(has_not);
    if (logical_count > 1) {
        throw std::invalid_argument(path +
                                    " must contain only one logical operator");
    }

    if (logical_count == 1) {
        if (expr.size() != 1) {
            throw std::invalid_argument(path +
                                        " cannot mix logical operators with leaf conditions");
        }
        MatchExpr out;
        if (has_any || has_all) {
            const char* key = has_any ? "any" : "all";
            const json& children = expr.at(key);
            if (!children.is_array() || children.empty()) {
                throw std::invalid_argument(path + "." + key +
                                            " must be a non-empty array");
            }
            out.op = has_any ? MatchExpr::Op::Any : MatchExpr::Op::All;
            out.children.reserve(children.size());
            for (std::size_t i = 0; i < children.size(); ++i) {
                out.children.push_back(parse_match_expr(
                    children[i], classifiers, path_index(path + "." + key, i), depth + 1));
            }
            return out;
        }

        out.op = MatchExpr::Op::Not;
        out.children.push_back(parse_match_expr(
            expr.at("not"), classifiers, path + ".not", depth + 1));
        return out;
    }

    validate_leaf(expr, classifiers, path);
    MatchExpr out;
    out.op = MatchExpr::Op::Leaf;
    out.leaf = expr;
    return out;
}

json parse_classifier_configs(const json& routing,
                              const std::set<std::string>& declared,
                              const RoutingPolicyParseOptions& options) {
    if (!routing.contains("classifiers")) {
        return json::array();
    }
    const json& classifiers = routing.at("classifiers");
    if (!classifiers.is_array()) {
        throw std::invalid_argument("routing.classifiers must be an array");
    }

    json resolved = json::array();
    for (std::size_t i = 0; i < classifiers.size(); ++i) {
        const std::string path = path_index("routing.classifiers", i);
        const json& classifier = classifiers[i];
        reject_unknown_keys(classifier, routing_classifier_keys(), path);
        required_string(classifier, "id", path);
        const std::string type = required_string(classifier, "type", path);

        if (classifier.contains("on_error")) {
            const std::string on_error = optional_string(classifier, "on_error", path);
            if (on_error != "match_true" && on_error != "match_false") {
                throw std::invalid_argument(path + ".on_error must be match_true or match_false");
            }
        }

        json item = classifier;
        if (classifier.contains("model")) {
            const std::string original = optional_string(classifier, "model", path);
            std::string resolved_model = resolve_component(original, options, path + ".model");
            require_declared(resolved_model, original, declared, options, path + ".model");

            // Fail at parse time, not at first request: a "classifier" model
            // must be able to answer /v1/classify (ModelType::CLASSIFICATION)
            // or serve as an LLM-as-classifier via chat_completion
            // (ModelType::LLM); a "semantic_similarity" model must be able to
            // embed (ModelType::EMBEDDING). Skipped when get_model_type isn't
            // configured (pure parser tests, and callers that don't have a
            // model registry to check against).
            if (options.get_model_type) {
                const std::optional<ModelType> resolved_type = options.get_model_type(resolved_model);
                if (!resolved_type) {
                    throw std::invalid_argument(
                        path + ".model '" + original + "' has an unresolvable type; "
                        "cannot validate it as a '" + type + "'");
                }
                const ModelType model_type = *resolved_type;
                if (type == "classifier" &&
                    model_type != ModelType::LLM && model_type != ModelType::CLASSIFICATION) {
                    throw std::invalid_argument(
                        path + ".model '" + original + "' has type '" +
                        model_type_to_string(model_type) +
                        "', which cannot serve as a classifier (needs an LLM or "
                        "classification model)");
                }
                if (type == "semantic_similarity" && model_type != ModelType::EMBEDDING) {
                    throw std::invalid_argument(
                        path + ".model '" + original + "' has type '" +
                        model_type_to_string(model_type) +
                        "', which cannot serve semantic_similarity (needs an embedding model)");
                }
            }

            item["model"] = std::move(resolved_model);
        }

        // make_classifier performs the type-specific checks (including the
        // `llm` router classifier implemented in #2405).
        resolved.push_back(std::move(item));
    }
    return resolved;
}

std::string parse_default_model(const json& routing,
                                const std::vector<std::string>& candidates,
                                const std::set<std::string>& declared,
                                const RoutingPolicyParseOptions& options) {
    const std::string original = required_string(routing, "default_model", "routing");
    std::string resolved = resolve_component(original, options, "routing.default_model");
    require_declared(resolved, original, declared, options, "routing.default_model");
    if (!contains_string(candidates, resolved)) {
        throw std::invalid_argument(
            "routing.default_model '" + original + "' must be listed in routing.candidates");
    }
    return resolved;
}

std::vector<Rule> parse_rules(const json& routing,
                              const std::vector<std::string>& candidates,
                              const std::map<std::string, ClassifierPtr>& classifiers,
                              const std::set<std::string>& declared,
                              const RoutingPolicyParseOptions& options) {
    if (routing.contains("router")) {
        reject_unknown_keys(routing.at("router"), routing_router_keys(), "routing.router");
        throw std::invalid_argument(
            "routing.router desugaring is reserved for #2405 and is not implemented by the M9 parser");
    }
    const json& rules_json = required_field(routing, "rules", "routing");
    if (!rules_json.is_array() || rules_json.empty()) {
        throw std::invalid_argument("routing.rules must be a non-empty array");
    }

    std::vector<Rule> rules;
    rules.reserve(rules_json.size());
    std::set<std::string> ids;
    for (std::size_t i = 0; i < rules_json.size(); ++i) {
        const std::string path = path_index("routing.rules", i);
        const json& rule_json = rules_json[i];
        reject_unknown_keys(rule_json, routing_rule_keys(), path);

        Rule rule;
        rule.id = required_rule_id(rule_json, "id", path);
        if (!ids.insert(rule.id).second) {
            throw std::invalid_argument(path + ".id duplicates rule id '" + rule.id + "'");
        }
        rule.match = parse_match_expr(
            required_field(rule_json, "match", path), classifiers, path + ".match", 0);

        const std::string original_route = required_string(rule_json, "route_to", path);
        rule.route_to = resolve_component(original_route, options, path + ".route_to");
        require_declared(rule.route_to, original_route, declared, options, path + ".route_to");
        if (!contains_string(candidates, rule.route_to)) {
            throw std::invalid_argument(path + ".route_to '" + original_route +
                                        "' must be listed in routing.candidates");
        }
        if (rule_json.contains("outputs")) {
            if (!rule_json.at("outputs").is_object()) {
                throw std::invalid_argument(path + ".outputs must be an object");
            }
            rule.outputs = rule_json.at("outputs");
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

} // namespace

const std::set<std::string>& routing_policy_root_keys() {
    static const std::set<std::string> keys = {
        "version", "model_name", "recipe", "components", "models", "routing"};
    return keys;
}

const std::set<std::string>& routing_block_keys() {
    static const std::set<std::string> keys = {
        "candidates", "default_model", "router", "classifiers", "rules"};
    return keys;
}

const std::set<std::string>& routing_router_keys() {
    static const std::set<std::string> keys = {"type", "model", "prompt"};
    return keys;
}

const std::set<std::string>& routing_classifier_keys() {
    static const std::set<std::string> keys = {
        "id", "type", "model", "prompt", "labels", "default_label",
        "reference_phrases", "on_error"};
    return keys;
}

const std::set<std::string>& routing_rule_keys() {
    static const std::set<std::string> keys = {"id", "match", "route_to", "outputs"};
    return keys;
}

const std::set<std::string>& routing_match_expr_keys() {
    static const std::set<std::string> keys = {
        "any", "all", "not", "classifier", "label", "min_score", "max_score",
        "keywords_any", "keywords_all", "regex", "min_chars", "max_chars",
        "has_tools", "has_images", "metadata"};
    return keys;
}

const std::set<std::string>& routing_metadata_match_keys() {
    static const std::set<std::string> keys = {"key", "equals", "any", "exists"};
    return keys;
}

// Desugar the `routing.router` (L0a) sugar into the explicit core form: one
// `llm` classifier whose labels are the candidates, plus one identity rule per
// candidate (label X -> route_to X), evaluated first-match-wins. The engine
// stays single-path; `routing.router` is pure load-time sugar. Returns a new
// `routing` object with `router` removed and `classifiers`/`rules` synthesized.
//
// Names: the synthesized classifier labels and rule match.labels use the
// AUTHORED candidate names (read from routing.candidates verbatim), because
// that is the vocabulary the author's prompt exposes to the router LLM and
// therefore what the model replies with. Only route_to is canonicalized — and
// that happens downstream in parse_rules via the component resolver, exactly as
// for hand-written rules. Feeding pre-resolved canonical IDs in here would
// break label matching (the LLM never sees canonical IDs) and would
// double-resolve route_to.
json desugar_routing_router(const json& routing) {
    const json& router = routing.at("router");
    reject_unknown_keys(router, routing_router_keys(), "routing.router");

    const std::string type = required_string(router, "type", "routing.router");
    if (type != "llm") {
        throw std::invalid_argument("routing.router.type must be 'llm'");
    }
    // The sugar owns classifiers + rules; authoring both forms is ambiguous.
    if (routing.contains("classifiers") || routing.contains("rules")) {
        throw std::invalid_argument(
            "routing.router cannot be combined with routing.classifiers or routing.rules");
    }
    required_string(router, "model", "routing.router");
    required_string(router, "prompt", "routing.router");

    // Authored names, validated already by parse_candidates (shape, non-empty,
    // declared, no duplicates after resolution) before this runs.
    std::vector<std::string> authored;
    for (const auto& entry : routing.at("candidates")) {
        authored.push_back(entry.get<std::string>());
    }

    json out = routing;
    out.erase("router");

    // The single llm classifier. `model` is resolved later by
    // parse_classifier_configs; labels are the candidates it must choose among.
    out["classifiers"] = json::array({json{
        {"id", "__router"},
        {"type", "llm"},
        {"model", router.at("model")},
        {"prompt", router.at("prompt")},
        {"labels", authored},
    }});

    // Identity rules. Index-based ids avoid any charset assumptions about
    // candidate names (which may contain '.' or '-').
    json rules = json::array();
    for (std::size_t i = 0; i < authored.size(); ++i) {
        rules.push_back(json{
            {"id", "__route_" + std::to_string(i)},
            {"match", json{
                {"classifier", "__router"},
                {"label", authored[i]},
                {"min_score", 1.0},
            }},
            {"route_to", authored[i]},
        });
    }
    out["rules"] = std::move(rules);
    return out;
}

RoutePolicy parse_route_policy_collection(const json& collection_json,
                                          const RoutingPolicyParseOptions& options) {
    reject_unknown_keys(collection_json, routing_policy_root_keys(), "collection");
    validate_version_1(collection_json);

    const std::string recipe = required_string(collection_json, "recipe", "collection");
    if (!is_router_collection_recipe(recipe)) {
        throw std::invalid_argument(
            "collection.recipe must be 'collection.router' for a routing policy");
    }

    const std::set<std::string> declared = parse_declared_components(collection_json, options);
    const json& routing = required_field(collection_json, "routing", "collection");
    reject_unknown_keys(routing, routing_block_keys(), "routing");

    RoutePolicy policy;
    policy.candidates = parse_candidates(routing, declared, options);
    policy.default_model = parse_default_model(routing, policy.candidates, declared, options);

    // Desugar the L0a `routing.router` sugar into explicit classifiers + rules
    // before the normal parse path runs. Everything downstream sees the core
    // form only.
    json desugared;
    const json* routing_eff = &routing;
    if (routing.contains("router")) {
        desugared = desugar_routing_router(routing);
        routing_eff = &desugared;
    }

    const json classifier_configs = parse_classifier_configs(*routing_eff, declared, options);
    policy.classifiers = make_classifiers(classifier_configs);
    policy.rules = parse_rules(*routing_eff, policy.candidates, policy.classifiers, declared, options);
    return policy;
}

} // namespace lemon
