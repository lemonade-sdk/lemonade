#include <lemon/utils/aixlog.hpp>

#include "lemon/route_decision_response.h"
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
            "id": {"type": "string"},
            "object": {"const": "response"},
            "created_at": {"type": "number"},
            "model": {"type": "string"},
            "status": {"type": "string"},
            "output": {"type": "array", "items": {"type": "object"},
                       "description": "Output items; a message item carries its text in content[].text."},
            "usage": {"type": "object"}
        }
    })");
    schema["properties"]["x_lemonade_route"] = route_decision_field_schema();
    return schema;
}

ordered_json response_event_schema() {
    return ordered_json::parse(R"({
        "type": "object",
        "required": ["type"],
        "properties": {
            "type": {"type": "string", "description": "Event type, one of the backend's OpenAI event types such as response.created, response.output_text.delta or response.completed."},
            "response": {"type": "object", "description": "response.created and response.completed: the response so far."},
            "delta": {"type": "string", "description": "response.output_text.delta: the next text."}
        }
    })");
}

class ResponsesRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.responses";
        s.methods = {"POST"};
        s.paths = {"responses"};
        s.summary = "Responses API";
        s.modality = "prompt/messages -> event";
        s.description =
            "Generates a model response to an input, in OpenAI's Responses format, loading the "
            "model on first use.";
        s.notes = {
            "With `stream: true`, the response is the backend's typed server-sent events, such as "
            "`response.created`, `response.output_text.delta` and `response.completed`; see "
            "OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming).",
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
        };
        s.args = {
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "The input, as a string or an array of input items."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to use for the response."},
            {"max_output_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "The maximum number of output tokens to generate."},
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
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If true, tokens will be sent as they are generated. If false, the response will be "
             "sent as a single message once complete. Defaults to false."},
            {"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension for `collection.router` models: `true` adds the routing "
             "decision to the response as `x_lemonade_route`."},
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
