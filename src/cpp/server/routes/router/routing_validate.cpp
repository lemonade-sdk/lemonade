#include <optional>
#include <string>

#include "lemon/error_types.h"
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
const RouteError kAmbiguousBody{400, "",
    "More than one of `messages`, `input` and `prompt` is sent, or `has_images` or `has_tools` "
    "is sent with `messages`, `input`, `tools` or an array `prompt`"};
const RouteError kInvalidContent{400, "",
    "A `messages` or `input` item's `content` is neither a string nor an array of objects, or "
    "a text part's `text` is not a string"};

const RouteArg kPolicy{"policy", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
                       "A `collection.router` policy document. `model_name` is accepted but is not required "
                       "for validation. See [Router Policies](../dev/router-policy.md)."};
const RouteArg kPrompt{"prompt", ArgIn::JsonBody,
                       {{"type", json::array({"string", "array"})}, {"items", {{"type", "string"}}}},
                       false, Support::Available,
                       "The prompt text to route, either a string or an array of strings. A string is a "
                       "single-turn preview. An array is read like a completions `prompt`, its strings "
                       "joined as separate lines, and counts as a request body. Any other type, or an "
                       "array holding a non-string, returns 400. Defaults to `\"\"`."};
const RouteArg kMessages{"messages", ArgIn::JsonBody,
                         {{"type", "array"}, {"items", {{"type", "object"}}}}, false, Support::Available,
                         "A chat completions `messages` array, read the way a routed `chat/completions` "
                         "request is: the latest user turn is the routing text, and image parts set "
                         "`has_images`."};
const RouteArg kInput{"input", ArgIn::JsonBody,
                      {{"type", json::array({"string", "array"})},
                       {"items", {{"type", json::array({"string", "object"})}}}},
                      false, Support::Available,
                      "A Responses API `input`, read the way a routed `responses` request is."};
const RouteArg kTools{"tools", ArgIn::JsonBody,
                      {{"type", "array"}, {"items", {{"type", "object"}}}}, false, Support::Available,
                      "A `tools` array; a non-empty one sets `has_tools`. Counts as a request body."};
const RouteArg kHasImages{"has_images", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "Simulate a request carrying image input. Defaults to `false`. Cannot be "
                          "combined with a request body."};
const RouteArg kHasTools{"has_tools", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                         "Simulate a request carrying tool definitions. Defaults to `false`. Cannot be "
                         "combined with a request body."};
const RouteArg kMetadata{"metadata", ArgIn::JsonBody,
                         {{"type", "object"}, {"additionalProperties", {{"type", "string"}}}}, false, Support::Available,
                         "String-valued metadata pairs matched by `metadata` conditions."};

std::optional<std::string> content_error(const json& content, const char* field) {
    if (content.is_null() || content.is_string()) return std::nullopt;
    if (!content.is_array()) {
        return std::string("'") + field + "' content must be a string or an array";
    }
    for (const auto& part : content) {
        if (!part.is_object()) {
            return std::string("'") + field + "' content parts must be objects";
        }
        const std::string type = part.value("type", std::string());
        if ((type == "text" || type == "input_text") &&
            (!part.contains("text") || !part["text"].is_string())) {
            return std::string("'") + field + "' text parts must contain a string 'text'";
        }
    }
    return std::nullopt;
}

std::optional<std::string> body_content_error(const json& body) {
    if (body.contains("messages")) {
        for (const auto& message : body["messages"]) {
            if (!message.contains("content")) continue;
            if (auto error = content_error(message["content"], "messages")) return error;
        }
    }
    if (body.contains("input") && body["input"].is_array()) {
        for (const auto& item : body["input"]) {
            if (item.is_string()) continue;
            const json content = item.contains("content") ? item["content"] : json::array({item});
            if (auto error = content_error(content, "input")) return error;
        }
    }
    return std::nullopt;
}

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
            "evaluated locally. Model-backed conditions (`semantic_similarity`, `classifier`, "
            "`zero_shot` and `llm`, including `routing.router`) may load and run their models. A "
            "model failure follows the classifier's `on_error` policy (`match_false` by default), "
            "so routing continues to a later rule or to `default_model`. A residency conflict is "
            "the exception: when the classifier model cannot be resident alongside the models "
            "already loaded, the request returns 409 and no decision is made, because the policy "
            "cannot run on this machine in this combination.",
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
        s.notes.push_back(
            "Send either the preview fields (a string `prompt`, `has_images`, `has_tools`) or a "
            "request body (`messages`, `input`, `tools` or an array `prompt`). A request body is "
            "read by the same code that reads a routed request, so signals such as images in "
            "earlier turns and the length of the whole conversation come from the body itself.");
        s.args = {kPolicy, kPrompt, kMessages, kInput, kTools, kHasImages, kHasTools, kMetadata};

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
        example.name = "preview";
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
        RouteExample body_example;
        body_example.name = "request body";
        body_example.description =
            "A chat `messages` array with an image part, sent with a `tools` array, sets both "
            "`has_images` and `has_tools` from the body itself, so the rule that needs both "
            "matches.";
        body_example.format = ResponseFormat::Json;
        body_example.args = ordered_json::parse(R"({
            "policy": {
                "version": "1",
                "recipe": "collection.router",
                "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
                "routing": {
                    "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
                    "default_model": "Qwen3-8B-GGUF",
                    "rules": [
                        {
                            "id": "vision-and-tools-to-big",
                            "match": {"all": [{"has_images": true}, {"has_tools": true}]},
                            "route_to": "vllm.qwen3-32b"
                        }
                    ]
                }
            },
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "What is in this picture?"},
                        {"type": "image_url", "image_url": {"url": "data:image/png;base64,AAA"}}
                    ]
                }
            ],
            "tools": [
                {
                    "type": "function",
                    "function": {
                        "name": "lookup_product",
                        "parameters": {"type": "object", "properties": {}}
                    }
                }
            ]
        })");
        s.examples = {example, body_example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kAmbiguousBody, kInvalidContent,
                    kInvalidPolicy, ModelLoader::kRouterResidencyConflict};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const bool prompt_is_array = req.has(kPrompt) && req.arg(kPrompt).is_array();
        const bool real_body = req.has(kMessages) || req.has(kInput) || req.has(kTools) ||
                               prompt_is_array;
        const int primary_fields = static_cast<int>(req.has(kMessages)) +
                                   static_cast<int>(req.has(kInput)) +
                                   static_cast<int>(req.has(kPrompt));
        if (primary_fields > 1) {
            write_plain_error(res, kAmbiguousBody,
                              "only one of 'messages', 'input', or 'prompt' may be provided");
            return;
        }
        if (real_body && (req.has(kHasImages) || req.has(kHasTools))) {
            write_plain_error(res, kAmbiguousBody,
                              "cannot combine 'has_images'/'has_tools' with a request body "
                              "('messages', 'input', 'tools', or an array 'prompt')");
            return;
        }
        if (auto error = body_content_error(req.body)) {
            write_plain_error(res, kInvalidContent, *error);
            return;
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

            RouteContext route_context = build_route_context(req.body, std::string());
            if (!real_body) {
                route_context.params.has_images = req.value(kHasImages, false);
                route_context.params.has_tools = req.value(kHasTools, false);
            }

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
        } catch (const RouterResidencyConflictException& e) {
            // The policy document is well-formed; the hardware just cannot host its
            // classifier right now. Reporting that as 400 would send the caller off
            // to debug a document that has nothing wrong with it.
            write_openai_error(res, ModelLoader::kRouterResidencyConflict, e.what(),
                               ErrorType::ROUTER_RESIDENCY_CONFLICT);
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
