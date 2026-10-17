#include <map>
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

const RouteError kInvalidPolicy{400, "", "`policy` is invalid or internally inconsistent"};

const RouteArg kPolicy{"policy", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
                       "A `collection.router` policy document. `model_name` is accepted but is not required "
                       "for validation. See [Router Policies](../dev/router-policy.md)."};
const RouteArg kPrompt{"prompt", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                       "The prompt text to route. Defaults to `\"\"`."};
const RouteArg kHasImages{"has_images", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "Simulate a request carrying image input. Defaults to `false`."};
const RouteArg kHasTools{"has_tools", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                         "Simulate a request carrying tool definitions. Defaults to `false`."};
const RouteArg kMetadata{"metadata", ArgIn::JsonBody,
                         {{"type", "object"}, {"additionalProperties", {{"type", "string"}}}}, false, Support::Available,
                         "String-valued metadata pairs matched by `metadata` conditions."};

class RoutingValidateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "router.routing_validate";
        s.methods = {"POST"};
        s.paths = {"routing/validate"};
        s.summary = "Test a routing policy against a prompt without registering it";
        s.experimental = true;
        s.description =
            "Returns the decision a routing policy makes for a prompt, without registering the "
            "policy or sending the request to the chosen candidate. The Router Builder's **Test "
            "Prompt** tab uses it to iterate on a policy before attaching it to a "
            "`collection.router` model.";
        s.notes = {
            "Validation is structural: every `candidates` entry, `default_model`, rule `route_to` "
            "and classifier model must be listed in `components`. Component names are not looked "
            "up in the model registry, so a policy can be tested before its candidates are "
            "downloaded, and registration's model-type checks, such as whether a "
            "`semantic_similarity` model can embed, do not run.",
            "Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, etc.) are "
            "evaluated locally. Model-backed conditions (`semantic_similarity`, `classifier` and "
            "`llm`, including `routing.router`) may load and run their models. A model failure "
            "follows the classifier's `on_error` policy (`match_false` by default), so routing "
            "continues to a later rule or to `default_model`.",
            "The example below uses explicit `routing.rules`, so its `normalized_policy` comes "
            "back unchanged. The `routing.router` shorthand is "
            "expanded into an `llm` classifier plus one rule per candidate, so a `routing` block "
            "written as:\n"
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
            "comes back with `router` replaced by these `classifiers` and `rules`:\n"
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
            "Match `decision.matched_rule` against this document: a policy written with only "
            "`routing.router` has no rules of its own, only the generated `__route_0`, "
            "`__route_1`, etc.",
        };
        s.args = {kPolicy, kPrompt, kHasImages, kHasTools, kMetadata};

        const ordered_json decision = {
            {"type", "object"},
            {"description",
             "The routing decision for the prompt, with the per-condition `trace` included. See "
             "[Routing](#routing-collectionrouter) for the decision object."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"decision", "normalized_policy"})},
            {"properties", {
                {"decision", decision},
                {"normalized_policy", {{"type", "object"}, {"description", "The policy as evaluated, with any `routing.router` shorthand expanded."}}},
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
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kInvalidPolicy};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const std::string prompt = req.value(kPrompt, std::string());
        const bool has_images = req.value(kHasImages, false);
        const bool has_tools = req.value(kHasTools, false);

        std::map<std::string, std::string> metadata;
        if (req.has(kMetadata)) {
            const json& metadata_json = req.arg(kMetadata);
            for (auto it = metadata_json.begin(); it != metadata_json.end(); ++it) {
                metadata[it.key()] = it.value().get<std::string>();
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
                req.arg(kPolicy), options, &normalized_routing);

            ModelLoader* loader = ctx_.model_loader;
            ClassifierServices services = make_router_classifier_services(
                *ctx_.router, [loader](const std::string& m) {
                    loader->ensure_loaded(m, json::object(), LoadPurpose::RoutingDependency);
                });
            RoutingPolicyEngine engine(std::move(policy), std::move(services));

            RouteContext route_context;
            route_context.input = prompt;
            route_context.params.chars = prompt.size();
            // A test prompt is a single turn with no history, so its whole-request
            // total is the prompt itself. Leaving this at 0 would make every
            // min_total_chars rule untestable here.
            route_context.params.total_chars = prompt.size();
            route_context.params.has_images = has_images;
            route_context.params.has_tools = has_tools;
            route_context.metadata = std::move(metadata);

            Decision decision = engine.route(route_context, /*want_trace=*/true);

            // Echo the policy actually evaluated (routing.router desugared into explicit
            // classifiers and rules), so a caller can match decision.matched_rule against
            // a rule that exists.
            json normalized_policy = req.arg(kPolicy);
            normalized_policy["routing"] = std::move(normalized_routing);

            json response = {
                {"decision", route_decision_to_json(decision)},
                {"normalized_policy", std::move(normalized_policy)},
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_plain_error(res, kInvalidPolicy, std::string("Invalid routing policy: ") + e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_routing_validate_route(ServerContext& ctx) {
    return std::make_unique<RoutingValidateRoute>(ctx);
}

} // namespace lemon
