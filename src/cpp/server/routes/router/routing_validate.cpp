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
            "Evaluates a routing policy document against a prompt and returns the decision the "
            "engine would make, without registering the policy or dispatching the request to the "
            "selected candidate. The Router Builder's **Test Prompt** tab uses it to iterate on a "
            "policy before attaching it to a `collection.router` model.";
        s.notes = {
            "The policy is validated structurally, as registration would: every `candidates` "
            "entry, `default_model`, rule `route_to` and classifier model must be listed in "
            "`components`. Component names are not looked up in the model registry, so a policy "
            "can be tested before its candidates are downloaded, and registration-time registry "
            "checks, such as whether a `semantic_similarity` model can embed, are not performed.",
            "Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, ...) are "
            "evaluated locally. Model-backed conditions (`semantic_similarity`, `classifier` and "
            "`llm`, including `routing.router`) may load and run their models. A model failure is "
            "handled by the classifier's `on_error` policy (`match_false` by default), so routing "
            "continues to a later rule or to `default_model` instead of failing.",
            "`decision` has the shape of the `x_lemonade_route` object a routed completion returns "
            "with `route_trace: true`, and always includes the trace. When no rule matches, "
            "`matched_rule` is empty, `default_used` is `true`, and `route_to` is the policy's "
            "`default_model`. `normalized_policy` is the policy as evaluated; see "
            "[Normalized Policies](#normalized-policies).",
            "A `400` answers invalid JSON, a missing or non-object `policy`, a non-string "
            "`prompt`, non-boolean `has_images` or `has_tools`, `metadata` that is not an object "
            "of strings, or an invalid policy, whose `error` starts with "
            "`Invalid routing policy:`.",
        };
        s.args = {
            {"policy", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
             "A `collection.router` policy document; see [Router Policies](../dev/router-policy.md). "
             "`model_name` is accepted but not required."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Prompt to route. Defaults to `\"\"`, which still exercises `min_chars` and any "
             "prompt-independent rules."},
            {"has_images", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Simulate a request carrying image input. Defaults to `false`."},
            {"has_tools", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Simulate a request carrying tool definitions. Defaults to `false`."},
            {"metadata", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "String-valued pairs matched by `metadata` conditions."},
        };

        // A routed completion carries the trace only with route_trace: true; this endpoint
        // always returns it.
        json decision = route_decision_schema();
        decision["properties"]["trace"]["description"] = "Per-condition trace of the evaluation.";
        decision["required"].push_back("trace");

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", json::array({"decision", "normalized_policy"})},
            {"properties", {
                {"decision", decision},
                {"normalized_policy", {
                    {"type", "object"},
                    {"description", "The policy as evaluated, with routing.router desugared into "
                                    "explicit classifiers and rules."}}},
            }},
        };
        response.example = json::parse(R"({
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
        s.responses = {response};
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
