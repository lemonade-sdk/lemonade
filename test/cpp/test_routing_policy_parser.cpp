// Unit tests for the Lemonade Router policy parser (#2383).
//
// Covers JSON -> RoutePolicy parsing, component canonicalization, validation
// errors, and schema/parser key parity against route_policy.schema.json.

#include "fake_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef ROUTING_FIXTURE_DIR
#define ROUTING_FIXTURE_DIR "test/cpp/fixtures/routing"
#endif

#ifndef ROUTING_SCHEMA_FILE
#define ROUTING_SCHEMA_FILE "src/cpp/resources/schemas/route_policy.schema.json"
#endif

using lemon::Decision;
using lemon::RouteContext;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::RoutingPolicyParseOptions;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static json load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

static json fixture(const std::string& name) {
    return load_json_file(std::string(ROUTING_FIXTURE_DIR) + "/" + name);
}

static RouteContext request(const std::string& input) {
    RouteContext ctx;
    ctx.input = input;
    ctx.params.chars = input.size();
    return ctx;
}

static bool throws_with(const json& doc, const std::string& expected) {
    try {
        lemon::parse_route_policy_collection(doc);
    } catch (const std::invalid_argument& e) {
        return std::string(e.what()).find(expected) != std::string::npos;
    } catch (...) {
        return false;
    }
    return false;
}

static bool throws_with_options(const json& doc, const RoutingPolicyParseOptions& options,
                                const std::string& expected) {
    try {
        lemon::parse_route_policy_collection(doc, options);
    } catch (const std::invalid_argument& e) {
        return std::string(e.what()).find(expected) != std::string::npos;
    } catch (...) {
        return false;
    }
    return false;
}

static bool parses_ok(const json& doc, const RoutingPolicyParseOptions& options) {
    try {
        lemon::parse_route_policy_collection(doc, options);
        return true;
    } catch (...) {
        return false;
    }
}

static std::set<std::string> schema_property_keys(const json& node) {
    std::set<std::string> keys;
    for (const auto& [key, _] : node.items()) {
        keys.insert(key);
    }
    return keys;
}

static void check_keys(const char* name,
                       const std::set<std::string>& actual,
                       const std::set<std::string>& expected) {
    if (actual == expected) {
        check(name, true);
        return;
    }
    std::printf("[FAIL] %s\n", name);
    std::printf("  parser keys:");
    for (const auto& key : actual) std::printf(" %s", key.c_str());
    std::printf("\n  schema keys:");
    for (const auto& key : expected) std::printf(" %s", key.c_str());
    std::printf("\n");
    ++g_failures;
}

static void test_parse_keywords_fixture_and_route() {
    json doc = fixture("l1_keywords.json");
    RoutePolicy policy = lemon::parse_route_policy_collection(doc);
    check("parser reads candidates", policy.candidates.size() == 2);
    check("parser reads default_model", policy.default_model == "Qwen3-8B-GGUF");
    check("parser reads rules", policy.rules.size() == 2);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyEngine engine(std::move(policy), fake.make());

    Decision code = engine.route(request("please fix this stack trace"), false);
    check("parsed deterministic rule routes matching request",
          code.route_to == "vllm.qwen3-32b" && code.matched_rule == "code-to-big");

    Decision plain = engine.route(request("hello"), false);
    check("parsed policy falls open to default",
          plain.route_to == "Qwen3-8B-GGUF" && plain.default_used);
}

static void test_component_resolver_canonicalizes_policy() {
    json doc = fixture("l1_keywords.json");
    RoutingPolicyParseOptions options;
    options.resolve_component = [](const std::string& name) -> std::optional<std::string> {
        if (name == "Qwen3-8B-GGUF") return "builtin.Qwen3-8B-GGUF";
        if (name == "vllm.qwen3-32b") return "user.vllm.qwen3-32b";
        return std::nullopt;
    };

    RoutePolicy policy = lemon::parse_route_policy_collection(doc, options);
    check("resolver canonicalizes candidates",
          policy.candidates[0] == "builtin.Qwen3-8B-GGUF" &&
          policy.candidates[1] == "user.vllm.qwen3-32b");
    check("resolver canonicalizes default_model",
          policy.default_model == "builtin.Qwen3-8B-GGUF");
    check("resolver canonicalizes route_to",
          policy.rules[0].route_to == "user.vllm.qwen3-32b");
}

static void test_validation_errors_are_clear() {
    json unknown_version = fixture("l1_keywords.json");
    unknown_version["version"] = "2";
    check("unknown schema major rejected clearly",
          throws_with(unknown_version, "Unsupported collection.router schema major"));

    json bad_route = fixture("l1_keywords.json");
    bad_route["routing"]["rules"][0]["route_to"] = "missing-model";
    check("unknown route_to component rejected",
          throws_with(bad_route, "not declared in collection.components"));

    json dangling_classifier = fixture("l2_semantic.json");
    dangling_classifier["routing"]["rules"][0]["match"]["classifier"] = "missing";
    check("dangling classifier reference rejected",
          throws_with(dangling_classifier, "unknown classifier"));

    json bad_band = fixture("l3_classifier.json");
    bad_band["routing"]["rules"][0]["match"]["any"][0]["min_score"] = 0.9;
    bad_band["routing"]["rules"][0]["match"]["any"][0]["max_score"] = 0.1;
    check("malformed score band rejected",
          throws_with(bad_band, "min_score greater than max_score"));

    json unsafe_rule_id = fixture("l1_keywords.json");
    unsafe_rule_id["routing"]["rules"][0]["id"] = "bad rule\r\nx-header";
    check("unsafe rule id rejected",
          throws_with(unsafe_rule_id, "must match [A-Za-z0-9._-]"));

    json router_plus_rules = fixture("l0a_llm_router.json");
    router_plus_rules["routing"]["rules"] = json::array({json{
        {"id", "r0"},
        {"match", {{"keywords_any", {"x"}}}},
        {"route_to", "Qwen3-8B-GGUF"}}});
    check("routing.router combined with explicit rules rejected",
          throws_with(router_plus_rules, "cannot be combined"));
}

// #2405: routing.router desugars at load time into one `llm` classifier plus
// identity rules. Uses a NON-identity resolver to pin the canonicalization
// contract: classifier labels stay the AUTHORED candidate names (that is the
// vocabulary the router LLM replies with), while route_to is resolved to
// canonical component IDs like any hand-written rule.
static void test_router_sugar_desugars_and_canonicalizes() {
    json doc = fixture("l0a_llm_router.json");
    RoutingPolicyParseOptions options;
    options.resolve_component = [](const std::string& name) -> std::optional<std::string> {
        return "builtin." + name;
    };

    RoutePolicy policy = lemon::parse_route_policy_collection(doc, options);
    check("router sugar parses successfully", policy.classifiers.size() == 1);
    check("router sugar synthesizes the __router llm classifier",
          policy.classifiers.count("__router") == 1 &&
              policy.classifiers.at("__router")->type() == "llm");

    const auto& labels = policy.classifiers.at("__router")->labels();
    check("classifier labels keep authored candidate names",
          labels.size() == 2 && labels[0] == "Qwen3-8B-GGUF" &&
              labels[1] == "Qwen3.5-35B-A3B-GGUF");

    check("one identity rule per candidate, route_to canonicalized",
          policy.rules.size() == 2 &&
              policy.rules[0].route_to == "builtin.Qwen3-8B-GGUF" &&
              policy.rules[1].route_to == "builtin.Qwen3.5-35B-A3B-GGUF");

    // End to end through the engine: the LLM replies with the AUTHORED name,
    // the decision routes to the CANONICAL id, and the trace identifies the
    // tested label. This is the regression the review called out: canonical
    // labels would make this reply unmatchable and silently fall to default.
    lemon::testing::FakeClassifierServices fake;
    fake.set_chat_reply("builtin.Qwen3-1.7B-GGUF",
        "{\"model\": \"Qwen3.5-35B-A3B-GGUF\", \"rationale\": \"Hard reasoning\"}");
    RoutingPolicyEngine engine(std::move(policy), fake.make());
    Decision decision = engine.route(request("prove this theorem"), true);
    check("authored reply routes to canonical candidate",
          decision.route_to == "builtin.Qwen3.5-35B-A3B-GGUF" && !decision.default_used);

    bool winning_label_traced = false;
    for (const auto& entry : decision.trace) {
        if (entry.condition == "classifier:__router" && entry.result &&
            entry.label == "Qwen3.5-35B-A3B-GGUF") {
            winning_label_traced = true;
        }
    }
    check("trace identifies the tested label for the winning rule", winning_label_traced);
}

static void test_classifier_capability_validation() {
    using lemon::ModelType;

    {
        json doc = fixture("l3_classifier.json");
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string& name) -> std::optional<ModelType> {
            if (name == "pii-detector-small") return ModelType::CLASSIFICATION;
            return ModelType::LLM;
        };
        check("classifier accepts CLASSIFICATION and LLM model types", parses_ok(doc, options));
    }
    {
        json doc = fixture("l3_classifier.json");
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string& name) -> std::optional<ModelType> {
            if (name == "pii-detector-small") return ModelType::EMBEDDING;
            return ModelType::LLM;
        };
        check("classifier rejects a model typed EMBEDDING",
              throws_with_options(doc, options, "cannot serve as a classifier"));
    }
    {
        json doc = fixture("l2_semantic.json");
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string&) -> std::optional<ModelType> {
            return ModelType::EMBEDDING;
        };
        check("semantic_similarity accepts an EMBEDDING model type", parses_ok(doc, options));
    }
    {
        json doc = fixture("l2_semantic.json");
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string&) -> std::optional<ModelType> {
            return ModelType::LLM;
        };
        check("semantic_similarity rejects a model typed LLM",
              throws_with_options(doc, options, "cannot serve semantic_similarity"));
    }
    {
        json doc = fixture("l3_classifier.json");
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string&) -> std::optional<ModelType> {
            return std::nullopt;
        };
        check("unresolvable model type is rejected, not silently accepted",
              throws_with_options(doc, options, "unresolvable type"));
    }
}

// Regression pair from PR review (fl0rianr): an inline collection component
// that isn't registered yet must have its type derived from its own inline
// definition (e.g. declared `labels`), not defaulted to a fixed type — a
// fixed-default fallback both rejects valid configs and accepts invalid ones,
// depending on which type it defaults to. This exercises the parser's side of
// that contract: as long as the resolver reports the inline definition's real
// type instead of guessing, both directions behave correctly.
static void test_inline_component_type_regression_pair() {
    using lemon::ModelType;

    json semantic_doc = fixture("l2_semantic.json");
    semantic_doc["components"] = json::array({"Qwen3-8B-GGUF", "vllm.qwen3-32b", "inline-embedder"});
    semantic_doc["routing"]["classifiers"][0]["model"] = "inline-embedder";
    {
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string& name) -> std::optional<ModelType> {
            if (name == "inline-embedder") return ModelType::EMBEDDING;
            return ModelType::LLM;
        };
        check("inline semantic_similarity + inline embedding definition passes",
              parses_ok(semantic_doc, options));
    }

    json classifier_doc = fixture("l3_classifier.json");
    classifier_doc["components"] = json::array(
        {"Qwen3-8B-GGUF", "vllm.qwen3-32b", "inline-embedder", "jailbreak-detector-small"});
    classifier_doc["routing"]["classifiers"][0]["model"] = "inline-embedder";
    {
        RoutingPolicyParseOptions options;
        options.get_model_type = [](const std::string& name) -> std::optional<ModelType> {
            if (name == "inline-embedder") return ModelType::EMBEDDING;
            return ModelType::LLM;
        };
        check("inline classifier + that same inline embedding definition fails",
              throws_with_options(classifier_doc, options, "cannot serve as a classifier"));
    }
}

static void test_schema_parser_key_parity() {
    json schema = load_json_file(ROUTING_SCHEMA_FILE);
    check_keys("root keys match schema",
               lemon::routing_policy_root_keys(),
               schema_property_keys(schema["properties"]));
    check_keys("routing keys match schema",
               lemon::routing_block_keys(),
               schema_property_keys(schema["$defs"]["routing"]["properties"]));
    check_keys("router sugar keys match schema",
               lemon::routing_router_keys(),
               schema_property_keys(schema["$defs"]["router_sugar"]["properties"]));
    check_keys("classifier keys match schema",
               lemon::routing_classifier_keys(),
               schema_property_keys(schema["$defs"]["classifier"]["properties"]));
    check_keys("rule keys match schema",
               lemon::routing_rule_keys(),
               schema_property_keys(schema["$defs"]["rule"]["properties"]));
    check_keys("match expr keys match schema",
               lemon::routing_match_expr_keys(),
               schema_property_keys(schema["$defs"]["match_expr"]["properties"]));
    check_keys("metadata keys match schema",
               lemon::routing_metadata_match_keys(),
               schema_property_keys(schema["$defs"]["metadata_match"]["properties"]));
}

int main() {
    test_parse_keywords_fixture_and_route();
    test_component_resolver_canonicalizes_policy();
    test_validation_errors_are_clear();
    test_router_sugar_desugars_and_canonicalizes();
    test_classifier_capability_validation();
    test_inline_component_type_regression_pair();
    test_schema_parser_key_parity();

    if (g_failures == 0) {
        std::printf("All routing policy parser tests passed.\n");
    } else {
        std::printf("%d routing policy parser test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
