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

    json router_sugar = fixture("l0a_llm_router.json");
    check("routing.router is recognized but deferred to #2405",
          throws_with(router_sugar, "routing.router desugaring"));
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
    test_schema_parser_key_parity();

    if (g_failures == 0) {
        std::printf("All routing policy parser tests passed.\n");
    } else {
        std::printf("%d routing policy parser test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
