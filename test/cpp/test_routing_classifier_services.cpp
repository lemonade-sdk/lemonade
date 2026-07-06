// Unit tests for Router-backed ClassifierServices wiring (#2384).

#include "lemon/routing_classifier_services.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using lemon::ClassifierContext;
using lemon::ClassifierPtr;
using lemon::Decision;
using lemon::MatchExpr;
using lemon::RouteContext;
using lemon::RoutePolicy;
using lemon::RoutingPolicyEngine;
using lemon::Rule;
using lemon::Score;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

static RouteContext route_context(const std::string& input) {
    RouteContext ctx;
    ctx.input = input;
    ctx.params.chars = input.size();
    return ctx;
}

static MatchExpr leaf(json value) {
    MatchExpr expr;
    expr.op = MatchExpr::Op::Leaf;
    expr.leaf = std::move(value);
    return expr;
}

static Rule rule(const std::string& id, MatchExpr match, const std::string& route_to) {
    Rule out;
    out.id = id;
    out.match = std::move(match);
    out.route_to = route_to;
    return out;
}

static ClassifierPtr make_semantic_classifier() {
    return lemon::make_classifier(json{
        {"id", "topic"},
        {"type", "semantic_similarity"},
        {"model", "embedder"},
        {"reference_phrases", {
            {"coding", {"write code"}},
            {"math", {"integral"}},
        }},
    });
}

static ClassifierPtr make_model_classifier() {
    return lemon::make_classifier(json{
        {"id", "pii"},
        {"type", "classifier"},
        {"model", "pii-model"},
        {"labels", {"PII", "NO_PII"}},
        {"default_label", "PII"},
    });
}

static void test_embed_uses_router_embeddings_shape() {
    std::vector<std::string> loaded;
    json seen_request;
    auto services = lemon::make_classifier_services_from_router_calls(
        [&](const json& request) {
            seen_request = request;
            return json{{"data", json::array({json{{"embedding", {1.0, 2.0, 3.0}}}})}};
        },
        [](const json&) { return json::object(); },
        [&](const std::string& model) { loaded.push_back(model); });

    auto vec = services.embed("embedder", "hello");
    check("embed ensure_loaded is called", loaded == std::vector<std::string>{"embedder"});
    check("embed forwards model and input",
          seen_request.value("model", "") == "embedder" &&
          seen_request.value("input", "") == "hello");
    check("embed parses OpenAI embedding response",
          vec.size() == 3 && vec[0] == 1.0f && vec[2] == 3.0f);
}

static void test_semantic_similarity_loops_through_router_embeddings() {
    std::map<std::string, std::vector<float>> vectors = {
        {"write code", {1.0f, 0.0f}},
        {"integral", {0.0f, 1.0f}},
        {"fix bug", {1.0f, 0.0f}},
    };
    int embedding_calls = 0;
    auto services = lemon::make_classifier_services_from_router_calls(
        [&](const json& request) {
            ++embedding_calls;
            const std::string text = request.value("input", "");
            return json{{"data", json::array({json{{"embedding", vectors.at(text)}}})}};
        },
        [](const json&) { return json::object(); });

    auto classifier = make_semantic_classifier();
    Score score = classifier->evaluate(ClassifierContext{
        route_context("fix bug"),
        services,
    });

    check("semantic_similarity returns scores through Router embeddings",
          score.ok && near(score.score_of("coding"), 1.0) &&
          near(score.score_of("math"), 0.0));
    check("semantic_similarity embeds references plus input", embedding_calls == 3);
}

static void test_run_classifier_uses_router_chat_completion() {
    std::vector<std::string> loaded;
    json seen_request;
    auto services = lemon::make_classifier_services_from_router_calls(
        [](const json&) { return json::object(); },
        [&](const json& request) {
            seen_request = request;
            return json{{"choices", json::array({
                json{{"message", {{"content", R"({"PII":0.85,"NO_PII":0.15})"}}}}
            })}};
        },
        [&](const std::string& model) { loaded.push_back(model); });

    auto scores = services.run_classifier("pii-model", "my ssn is 123");
    check("run_classifier ensure_loaded is called",
          loaded == std::vector<std::string>{"pii-model"});
    check("run_classifier forwards model and user input",
          seen_request.value("model", "") == "pii-model" &&
          seen_request["messages"][1].value("content", "") == "my ssn is 123");
    check("run_classifier parses chat JSON label scores",
          near(scores.at("PII"), 0.85) && near(scores.at("NO_PII"), 0.15));
}

static void test_run_classifier_ignores_openai_metadata() {
    auto services = lemon::make_classifier_services_from_router_calls(
        [](const json&) { return json::object(); },
        [](const json&) {
            return json{
                {"id", "chatcmpl-abc123"},
                {"object", "chat.completion"},
                {"created", 1742927481},
                {"model", "pii-model"},
                {"choices", json::array({
                    json{
                        {"index", 0},
                        {"finish_reason", "stop"},
                        {"message", {{"role", "assistant"},
                                     {"content", R"({"PII":0.9,"NO_PII":0.1})"}}},
                    }
                })},
                {"usage", {{"prompt_tokens", 12}, {"completion_tokens", 8},
                           {"total_tokens", 20}}},
            };
        });

    auto scores = services.run_classifier("pii-model", "my ssn is 123");
    check("run_classifier ignores OpenAI metadata fields",
          scores.find("created") == scores.end() &&
          scores.find("prompt_tokens") == scores.end());
    check("run_classifier reads scores from message content",
          scores.size() == 2 && near(scores.at("PII"), 0.9) &&
          near(scores.at("NO_PII"), 0.1));
}


static void test_model_classifier_routes_with_router_services() {
    auto services = lemon::make_classifier_services_from_router_calls(
        [](const json&) { return json::object(); },
        [](const json&) {
            return json{{"choices", json::array({
                json{{"message", {{"content", R"({"PII":0.9,"NO_PII":0.1})"}}}}
            })}};
        });

    RoutePolicy policy;
    policy.candidates = {"local", "cloud"};
    policy.default_model = "cloud";
    policy.classifiers["pii"] = make_model_classifier();
    policy.rules = {
        rule("keep-private", leaf(json{{"classifier", "pii"}, {"min_score", 0.5}}), "local"),
    };

    RoutingPolicyEngine engine(std::move(policy), services);
    Decision decision = engine.route(route_context("my ssn is 123"), true);
    check("model-backed classifier routes through Router services",
          decision.route_to == "local" && decision.matched_rule == "keep-private");
    check("model-backed classifier trace carries score",
          decision.trace.size() == 1 && decision.trace[0].score.has_value() &&
          near(*decision.trace[0].score, 0.9));
}

static void test_chat_service_extracts_text() {
    auto services = lemon::make_classifier_services_from_router_calls(
        [](const json&) { return json::object(); },
        [](const json& request) {
            check("chat forwards system prompt",
                  request["messages"][0].value("content", "") == "route this");
            return json{{"choices", json::array({
                json{{"message", {{"content", "large-model"}}}}
            })}};
        });

    check("chat extracts assistant content",
          services.chat("router-model", "route this", "hard problem") == "large-model");
}

int main() {
    test_embed_uses_router_embeddings_shape();
    test_semantic_similarity_loops_through_router_embeddings();
    test_run_classifier_uses_router_chat_completion();
    test_run_classifier_ignores_openai_metadata();
    test_model_classifier_routes_with_router_services();
    test_chat_service_extracts_text();

    if (g_failures == 0) {
        std::printf("All routing classifier service tests passed.\n");
    } else {
        std::printf("%d routing classifier service test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
