#include <optional>
#include <string>

#include "lemon/model_residency.h"
#include "lemon/route_decision_response.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class RoutingValidateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "router.routing_validate";
        s.methods = {"POST"};
        s.paths = {"routing/validate"};
        s.summary = "Evaluate an ad-hoc routing policy against a prompt without registering it";
        s.experimental = true;
        s.description =
            "Evaluate a routing policy document against a prompt and return the decision the\n"
            "engine would make, without registering the policy or dispatching the user\n"
            "request to the selected candidate. This is the endpoint behind the Router\n"
            "Builder's **Test Prompt** tab: it lets a policy be iterated on before it is\n"
            "attached to a `collection.router` model.\n"
            "\n"
            "The endpoint performs parser-level structural policy validation: every\n"
            "`candidates` entry, `default_model`, rule `route_to`, and classifier model must\n"
            "be listed in `components`. It does not consult the live model registry:\n"
            "component names are accepted as-is, so a policy can be tested before its\n"
            "candidates are downloaded. Because names and component model types are not\n"
            "resolved through the registry, registration-time registry checks (for example,\n"
            "whether a `semantic_similarity` model can embed or a `classifier` model can\n"
            "classify/chat) are not performed by this endpoint.\n"
            "\n"
            "Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, …)\n"
            "are evaluated locally. Model-backed conditions (`semantic_similarity`,\n"
            "`classifier`, and `llm`, including `routing.router`) may load and run their\n"
            "referenced models. A model-evaluation failure is handled by the classifier's\n"
            "`on_error` policy (`match_false` by default), so routing normally continues to a\n"
            "later rule or falls through to `default_model` rather than treating the policy\n"
            "as invalid.";
        s.notes = {
            "`decision` has the same shape as the `x_lemonade_route` object a routed\n"
            "completion returns with `route_trace: true`, and the trace is always included\n"
            "here. When no rule matches, `matched_rule` is empty, `default_used` is `true`,\n"
            "and `route_to` is the policy's `default_model`.",
            "`normalized_policy` echoes the policy as it was actually evaluated. The policy\n"
            "above uses explicit `routing.rules`, so it comes back unchanged. The field earns\n"
            "its place when a policy uses the `routing.router` shorthand: that sugar is\n"
            "desugared into an explicit `llm` classifier plus one identity rule per\n"
            "candidate, so a `routing` block authored as:\n"
            "\n"
            "```json\n"
            "{\n"
            "  \"router\": {\n"
            "    \"type\": \"llm\",\n"
            "    \"model\": \"Qwen3-8B-GGUF\",\n"
            "    \"prompt\": \"Pick the best model for this request.\"\n"
            "  },\n"
            "  \"candidates\": [\"Qwen3-8B-GGUF\", \"vllm.qwen3-32b\"],\n"
            "  \"default_model\": \"Qwen3-8B-GGUF\"\n"
            "}\n"
            "```\n"
            "\n"
            "is echoed back with `router` removed and synthesized `classifiers`/`rules`:\n"
            "\n"
            "```json\n"
            "{\n"
            "  \"candidates\": [\"Qwen3-8B-GGUF\", \"vllm.qwen3-32b\"],\n"
            "  \"default_model\": \"Qwen3-8B-GGUF\",\n"
            "  \"classifiers\": [\n"
            "    {\n"
            "      \"id\": \"__router\",\n"
            "      \"type\": \"llm\",\n"
            "      \"model\": \"Qwen3-8B-GGUF\",\n"
            "      \"prompt\": \"Pick the best model for this request.\",\n"
            "      \"labels\": [\"Qwen3-8B-GGUF\", \"vllm.qwen3-32b\"]\n"
            "    }\n"
            "  ],\n"
            "  \"rules\": [\n"
            "    {\n"
            "      \"id\": \"__route_0\",\n"
            "      \"match\": {\"classifier\": \"__router\", \"label\": \"Qwen3-8B-GGUF\", \"min_score\": 1.0},\n"
            "      \"route_to\": \"Qwen3-8B-GGUF\"\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"__route_1\",\n"
            "      \"match\": {\"classifier\": \"__router\", \"label\": \"vllm.qwen3-32b\", \"min_score\": 1.0},\n"
            "      \"route_to\": \"vllm.qwen3-32b\"\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "```\n"
            "\n"
            "Match `decision.matched_rule` against this document rather than the one you\n"
            "sent — a policy authored with only `routing.router` has no `routing.rules` of\n"
            "its own, only the synthesized `__route_0`, `__route_1`, … rules shown here.",
            "A `400` answers a body that is not valid JSON, a missing or non-object `policy`, a "
            "non-string `prompt`, non-boolean `has_images`/`has_tools`, a `metadata` that is not an "
            "object of string values, or a policy document that is invalid or internally "
            "inconsistent, whose `error` is prefixed with `Invalid routing policy:`.",
        };
        s.args = {
            {"policy", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
             "A `collection.router` policy document. `model_name` is accepted but is not required "
             "for validation. See [Router Policies](../dev/router-policy.md)."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "The prompt text to route. Defaults to `\"\"`, which still exercises `min_chars` (0 "
             "chars) and any prompt-independent rules."},
            {"has_images", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Simulate a request carrying image input. Default `false`."},
            {"has_tools", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Simulate a request carrying tool definitions. Default `false`."},
            {"metadata", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "String-valued metadata pairs matched by `metadata` conditions."},
        };

        // A routed completion carries the trace only with route_trace: true; this endpoint
        // always returns it.
        ordered_json decision = route_decision_schema();
        decision["properties"]["trace"]["description"] = "Per-condition trace of the evaluation.";
        decision["required"].push_back("trace");

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"decision", "normalized_policy"})},
            {"properties", {
                {"decision", decision},
                {"normalized_policy", {{"type", "object"}}},
            }},
        };
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "policy": {
                "version": "1",
                "recipe": "collection.router",
                "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
                "routing": {
                    "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
                    "default_model": "Qwen3-8B-GGUF",
                    "rules": [
                        {
                            "id": "code-to-big",
                            "match": {"keywords_any": ["def ", "function", "compile"]},
                            "route_to": "vllm.qwen3-32b"
                        }
                    ]
                }
            },
            "prompt": "please write a def to reverse a list"
        })");
        s.examples = {example};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;

        if (!request_json.contains("policy") || !request_json["policy"].is_object()) {
            write_plain_error(res, 400, "'policy' must be a JSON object");
            return;
        }
        if (request_json.contains("prompt") && !request_json["prompt"].is_string()) {
            write_plain_error(res, 400, "'prompt' must be a string");
            return;
        }
        const std::string prompt = request_json.value("prompt", std::string());

        if (request_json.contains("has_images") && !request_json["has_images"].is_boolean()) {
            write_plain_error(res, 400, "'has_images' must be a boolean");
            return;
        }
        const bool has_images = request_json.value("has_images", false);

        if (request_json.contains("has_tools") && !request_json["has_tools"].is_boolean()) {
            write_plain_error(res, 400, "'has_tools' must be a boolean");
            return;
        }
        const bool has_tools = request_json.value("has_tools", false);

        if (request_json.contains("metadata")) {
            const json& metadata_json = request_json["metadata"];
            if (!metadata_json.is_object()) {
                write_plain_error(res, 400, "'metadata' must be a JSON object of string values");
                return;
            }
            for (auto it = metadata_json.begin(); it != metadata_json.end(); ++it) {
                if (!it.value().is_string()) {
                    write_plain_error(res, 400,
                        "'metadata' value for key '" + it.key() + "' must be a string");
                    return;
                }
            }
        }

        try {
            // The policy is not attached to a registered model, so any component name is
            // accepted as-is. require_declared_components keeps its default, so the
            // document must still be internally consistent, as registration requires.
            RoutingPolicyParseOptions options;
            options.resolve_component = [](const std::string& name) -> std::optional<std::string> {
                return name;
            };
            json normalized_routing;
            RoutePolicy policy = parse_route_policy_collection(
                request_json["policy"], options, &normalized_routing);

            ModelLoader* loader = ctx_.model_loader;
            ClassifierServices services = make_router_classifier_services(
                *ctx_.router, [loader](const std::string& m) {
                    loader->ensure_loaded(m, json::object(), LoadPurpose::RoutingDependency);
                });
            RoutingPolicyEngine engine(std::move(policy), std::move(services));

            // A test prompt is one user turn with no history. Building it as a request
            // gives the engine the same context a real completion request would.
            json content = json::array({{{"type", "text"}, {"text", prompt}}});
            if (has_images) {
                content.push_back({{"type", "image_url"}, {"image_url", {{"url", ""}}}});
            }
            json test_request = {
                {"messages", json::array({{{"role", "user"}, {"content", content}}})},
            };
            if (has_tools) {
                test_request["tools"] = json::array({json::object()});
            }
            if (request_json.contains("metadata")) {
                test_request["metadata"] = request_json["metadata"];
            }
            RouteContext route_context = build_route_context(test_request, "");

            Decision decision = engine.route(route_context, /*want_trace=*/true);

            // Echo the policy actually evaluated (routing.router desugared into explicit
            // classifiers and rules), so a caller can match decision.matched_rule against
            // a rule that exists.
            json normalized_policy = request_json["policy"];
            normalized_policy["routing"] = std::move(normalized_routing);

            json response = {
                {"decision", route_decision_to_json(decision)},
                {"normalized_policy", std::move(normalized_policy)},
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_plain_error(res, 400, std::string("Invalid routing policy: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_routing_validate_route(ServerContext& ctx) {
    return std::make_unique<RoutingValidateRoute>(ctx);
}

} // namespace lemon
