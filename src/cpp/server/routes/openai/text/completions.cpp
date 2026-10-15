#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/streaming_proxy.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kUnparsableBody{500, "", "The body is empty or is not valid JSON"};
const RouteError kNotLlm{400, "", "`model` is not an LLM, which is checked after it loads"};
const RouteError kNoChoices{500, "", "The backend's response has no `choices`"};

ordered_json text_completion_schema(bool chunk) {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string", "description": "Identifier of the completion."},
            "object": {"const": "text_completion", "description": "Always `\"text_completion\"`."},
            "created": {"type": "integer", "description": "Unix timestamp of when the completion was created."},
            "model": {"type": "string", "description": "The model that generated the response."},
            "system_fingerprint": {"type": "string", "description": "Build identifier of the backend, when it reports one."},
            "choices": {"type": "array", "description": "The generated text, one entry per choice.", "items": {
                "type": "object",
                "required": ["index", "text"],
                "properties": {
                    "index": {"type": "integer", "description": "Position of the choice in `choices`."},
                    "text": {"type": "string", "description": "The generated text."},
                    "logprobs": {"description": "Log probabilities of the output tokens when the request sets `logprobs`, else `null`."},
                    "finish_reason": {"type": ["string", "null"], "description": "Why generation stopped, such as `stop` or `length`."}
                }
            }},
            "usage": {
                "type": "object",
                "description": "Token counts for the request.",
                "properties": {
                    "prompt_tokens": {"type": "integer", "description": "Tokens in the prompt."},
                    "completion_tokens": {"type": "integer", "description": "Tokens generated."},
                    "total_tokens": {"type": "integer", "description": "Sum of `prompt_tokens` and `completion_tokens`."},
                    "prompt_tokens_details": {"type": "object", "description": "Breakdown of the prompt tokens, such as `cached_tokens`, when the backend reports it."}
                }
            },
            "timings": {"type": "object", "description": "Prompt processing and generation timings, passed through from backends that report them, such as llama.cpp."}
        }
    })");
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    if (chunk) {
        schema["description"] = "A chunk of the completion text. The stream ends with `data: [DONE]`.";
        auto& properties = schema["properties"];
        properties["id"]["description"] = "Identifier of the completion, the same on every chunk.";
        properties["model"]["description"] =
            "The backend's name for the model, such as the GGUF file path for llama.cpp models.";
        properties["choices"]["description"] = "The next part of each choice's text.";
        properties["choices"]["items"]["properties"]["text"]["description"] =
            "The text generated since the previous chunk.";
        properties["choices"]["items"]["properties"]["finish_reason"]["description"] =
            "Why generation stopped, such as `stop` or `length`, on the choice's last chunk; "
            "`null` before it.";
        properties["usage"]["description"] = "Final chunk only: token counts for the request.";
        properties["timings"]["description"] =
            "Final chunk only: prompt processing and generation timings, passed through from "
            "backends that report them, such as llama.cpp.";
    }
    return schema;
}

class CompletionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.completions";
        s.methods = {"POST"};
        s.paths = {"completions"};
        s.summary = "Continue a text prompt";
        s.modality = "prompt -> completion";
        s.description =
            "Continues a text prompt, downloading and loading the model if it is not loaded.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
            "Once a stream has started, a backend error arrives as a `data: {\"error\": ...}` "
            "event in the `200` stream.",
        };
        s.args = {
            {"prompt", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "The text to continue, as a string or an array of strings."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to run."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` sends tokens as server-sent events as they are generated; `false` sends one "
             "response once complete. Defaults to `false`."},
            {"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
             "Sequences that stop generation, as a string or an array of strings. The returned "
             "text leaves them out."},
            {"echo", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Partial,
             "`true` asks the backend to return the prompt before the completion. llama.cpp "
             "models ignore it."},
            {"logprobs", ArgIn::JsonBody, {{"type", json::array({"boolean", "integer"})}}, false, Support::Available,
             "`true`, or a number of top alternatives per token, returns the log probability of each "
             "output token. Defaults to `false`."},
            {"temperature", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Sampling temperature."},
            {"repeat_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage "
             "repetition."},
            {"top_k", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of top tokens to consider during sampling."},
            {"top_p", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Cumulative probability of the top tokens to consider during nucleus sampling, from "
             "`0.0` to `1.0`."},
            {"max_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens."},
            {"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension for `collection.router` models: `true` adds the per-condition "
             "`trace` to `x_lemonade_route`. Defaults to `false`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse completion;
        completion.format = ResponseFormat::Json;
        completion.schema = text_completion_schema(false);

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = {{"oneOf", ordered_json::array({
            text_completion_schema(true), StreamingProxy::error_event_schema()})}};

        s.responses = {completion, stream};

        RouteExample completion_example;
        completion_example.name = "non-streaming";
        completion_example.format = ResponseFormat::Json;
        completion_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "What is the population of Paris?",
            "stream": false,
            "max_tokens": 64
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::EventStream;
        stream_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "prompt": "What is the population of Paris?",
            "stream": true,
            "max_tokens": 64
        })");

        s.examples = {completion_example, stream_example};
        s.errors = {kNotLlm, kBackendError, kNoChoices};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response&) override {
        // Router policies call backends here, before run() does.
        auto cancel = cancel_on_disconnect(req);
        req.route_decision = router_dispatch(ctx_, req);
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        if (ctx_.router->get_model_type(req.model) != ModelType::LLM) {
            LOG(ERROR, "Server") << "Model does not support completion" << std::endl;
            write_openai_error(res, kNotLlm, "This model does not support completion. Only LLM models support this endpoint.");
            return;
        }

        bool is_streaming = req.body.contains("stream") && req.body["stream"].get<bool>();
        if (is_streaming) {
            LOG(INFO, "Server") << "POST /api/v1/completions - Streaming" << std::endl;
            Router* router = ctx_.router;
            stream_response(req, res, [router](const std::string& body, httplib::DataSink& sink) {
                router->completion_stream(body, sink);
            });
            return;
        }

        auto cancel = cancel_on_disconnect(req);
        auto response = ctx_.router->completion(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Backend returned error response: " << response["error"].dump() << std::endl;
            set_error_response(response, res, kBackendError);
            return;
        }

        if (!response.contains("choices")) {
            LOG(ERROR, "Server") << "Response missing 'choices' field. Response: " << response.dump() << std::endl;
            write_plain_error(res, kNoChoices, "Backend returned invalid response format");
            return;
        }

        attach_route_decision(response, res, req.route_decision);
        res.set_content(response.dump(), "application/json");
        record_usage(response, req);
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_plain_error(res, kUnparsableBody, error.what());
    }

    std::vector<RouteError> invalid_body_errors() const override { return {kUnparsableBody}; }

    LoadSpan load_span() const override { return {"LLM", "completions"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_completions_route(ServerContext& ctx) {
    return std::make_unique<CompletionsRoute>(ctx);
}

} // namespace lemon
