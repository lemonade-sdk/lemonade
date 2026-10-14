#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/route_decision_response.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

ordered_json text_completion_schema(bool chunk) {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string"},
            "object": {"const": "text_completion"},
            "created": {"type": "integer"},
            "model": {"type": "string"},
            "choices": {"type": "array", "items": {
                "type": "object",
                "required": ["index", "text"],
                "properties": {
                    "index": {"type": "integer"},
                    "text": {"type": "string"},
                    "finish_reason": {"type": ["string", "null"]}
                }
            }},
            "usage": {"type": "object"}
        }
    })");
    schema["properties"]["x_lemonade_route"] = route_decision_field_schema();
    if (chunk) {
        schema["properties"]["usage"]["description"] = "Final chunk only: token usage.";
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
        s.summary = "Text Completions";
        s.modality = "prompt -> completion";
        s.description =
            "Continues a text prompt, loading the model on first use.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
        };
        s.args = {
            {"prompt", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "The prompt to use for the completion."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to use for the completion."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If true, tokens will be sent as they are generated. If false, the response will be "
             "sent as a single message once complete. Defaults to false."},
            {"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
             "Up to 4 sequences where the API will stop generating further tokens. The returned "
             "text will not contain the stop sequence. Can be a string or an array of strings."},
            {"echo", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Echo back the prompt in addition to the completion. Non-streaming only."},
            {"logprobs", ArgIn::JsonBody, {{"type", json::array({"boolean", "integer"})}}, false, Support::Available,
             "If true, returns the log probability of each output token. Defaults to false. Non-streaming only."},
            {"temperature", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "What sampling temperature to use."},
            {"repeat_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Number between 1.0 and 2.0. 1.0 means no penalty. Higher values discourage "
             "repetition."},
            {"top_k", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Integer that controls the number of top tokens to consider during sampling."},
            {"top_p", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Float between 0.0 and 1.0 that controls the cumulative probability of top tokens to "
             "consider during nucleus sampling."},
            {"max_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "An upper bound for the number of tokens that can be generated for a completion."},
            {"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension for `collection.router` models: `true` adds the routing "
             "decision to the response as `x_lemonade_route`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse completion;
        completion.format = ResponseFormat::Json;
        completion.schema = text_completion_schema(false);

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = text_completion_schema(true);

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
            write_openai_error(res, 400, "This model does not support completion. Only LLM models support this endpoint.");
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
            set_error_response(response, res);
            return;
        }

        if (!response.contains("choices")) {
            LOG(ERROR, "Server") << "Response missing 'choices' field. Response: " << response.dump() << std::endl;
            write_plain_error(res, 500, "Backend returned invalid response format");
            return;
        }

        attach_route_decision(response, res, req.route_decision);
        res.set_content(response.dump(), "application/json");
        record_usage(response, req);
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_plain_error(res, 500, error.what());
    }

    LoadSpan load_span() const override { return {"LLM", "completions"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_completions_route(ServerContext& ctx) {
    return std::make_unique<CompletionsRoute>(ctx);
}

} // namespace lemon
