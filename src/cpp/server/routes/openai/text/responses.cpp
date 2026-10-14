#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

ordered_json response_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created_at", "model", "output"],
        "properties": {
            "id": {"type": "string", "description": "Identifier of the response."},
            "object": {"const": "response", "description": "Always `\"response\"`."},
            "created_at": {"type": "number", "description": "Unix timestamp of when the response was created."},
            "completed_at": {"type": "number", "description": "Unix timestamp of when the response completed, when the backend reports it."},
            "model": {"type": "string", "description": "The model that generated the response."},
            "status": {"type": "string", "description": "State of the response, such as `completed`."},
            "output": {"type": "array", "items": {"type": "object"},
                       "description": "Output items; a message item carries its text in `content[].text`."},
            "usage": {
                "type": "object",
                "description": "Token counts for the request.",
                "properties": {
                    "input_tokens": {"type": "integer", "description": "Tokens in the input."},
                    "output_tokens": {"type": "integer", "description": "Tokens generated."},
                    "total_tokens": {"type": "integer", "description": "Sum of `input_tokens` and `output_tokens`."},
                    "input_tokens_details": {"type": "object", "description": "Breakdown of the input tokens, such as `cached_tokens`, when the backend reports it."}
                }
            }
        }
    })");
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    return schema;
}

ordered_json response_event_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["type"],
        "properties": {
            "type": {"type": "string", "description": "Event type, passed through from the backend, such as `response.created`, `response.output_text.delta` or `response.completed`; see OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming)."},
            "response": {"type": "object", "description": "Lifecycle events, such as `response.created`, `response.in_progress` and `response.completed`, only: the response so far."},
            "item": {"type": "object", "description": "Output item events, such as `response.output_item.added`, only: the output item."},
            "item_id": {"type": "string", "description": "Delta events only: the `id` of the output item the text belongs to."},
            "delta": {"type": "string", "description": "Delta events, such as `response.output_text.delta` and `response.reasoning_text.delta`, only: the next text."},
            "timings": {"type": "object", "description": "`response.completed` only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp."}
        }
    })");
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    return schema;
}

class ResponsesRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.responses";
        s.methods = {"POST"};
        s.paths = {"responses"};
        s.summary = "Generate a response in OpenAI's Responses format";
        s.modality = "prompt/messages -> event";
        s.description =
            "Generates a model response to an input, in OpenAI's Responses format, downloading and "
            "loading the model if it is not loaded.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
            "A `400` answers a missing `model`, or a model whose backend has no Responses API, "
            "such as a FastFlowLM, cloud or non-LLM model. An unknown model, or one not available "
            "on this system, answers `404`; a load blocked by pinned models or a routing residency "
            "conflict answers `409`; any other load failure, or invalid JSON, answers `500`. A "
            "backend error answers with the backend's status, except that once a stream has "
            "started it arrives as a `data: {\"error\": ...}` event in the `200` stream.",
        };
        s.args = {
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "The input, as a string or an array of input items."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to run."},
            {"max_output_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens."},
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
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` sends tokens as server-sent events as they are generated; `false` sends one "
             "response once complete. Defaults to `false`."},
            {"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension for `collection.router` models: `true` adds the per-condition "
             "`trace` to `x_lemonade_route`. Defaults to `false`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = response_schema();

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = response_event_schema();

        s.responses = {response, stream};

        RouteExample response_example;
        response_example.name = "non-streaming";
        response_example.format = ResponseFormat::Json;
        response_example.args = ordered_json::parse(R"({
            "model": "Llama-3.2-1B-Instruct-GGUF",
            "input": "What is the population of Paris?",
            "stream": false
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::EventStream;
        stream_example.args = ordered_json::parse(R"({
            "model": "Llama-3.2-1B-Instruct-GGUF",
            "input": "What is the population of Paris?",
            "stream": true
        })");

        s.examples = {response_example, stream_example};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response&) override {
        // Router policies call backends here, before run() does.
        auto cancel = cancel_on_disconnect(req);
        req.route_decision = router_dispatch(ctx_, req);
        if (req.body.contains("model")) {
            req.body["model"].get<std::string>();  // a non-string model fails before any load
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        bool is_streaming = req.body.contains("stream") && req.body["stream"].get<bool>();
        if (is_streaming) {
            LOG(INFO, "Server") << "POST /api/v1/responses - Streaming" << std::endl;
            Router* router = ctx_.router;
            stream_response(req, res, [router](const std::string& body, httplib::DataSink& sink) {
                router->responses_stream(body, sink);
            });
            return;
        }

        LOG(INFO, "Server") << "POST /api/v1/responses - Non-streaming" << std::endl;
        auto cancel = cancel_on_disconnect(req);
        auto response = ctx_.router->responses(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Responses backend error: " << response["error"].dump() << std::endl;
            set_error_response(response, res);
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
};

} // namespace

std::unique_ptr<ApiRoute> make_responses_route(ServerContext& ctx) {
    return std::make_unique<ResponsesRoute>(ctx);
}

} // namespace lemon
