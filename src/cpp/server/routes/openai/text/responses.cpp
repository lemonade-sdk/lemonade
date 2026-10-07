#include <lemon/utils/aixlog.hpp>

#include "lemon/route_decision_response.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json response_schema() {
    auto schema = json::parse(R"({
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
    schema["properties"]["x_lemonade_route"] = route_decision_schema();
    return schema;
}

json response_event_schema() {
    return json::parse(R"({
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
        s.description = "Generates a response to an input, loading the model on first use.";
        s.notes = {
            "Streaming relays the backend's semantic events as they arrive. llama.cpp sends "
            "OpenAI's event types, including `response.created`, `response.in_progress`, "
            "`response.output_item.added`, `response.reasoning_text.delta` for reasoning "
            "models, `response.output_text.delta`, `response.output_item.done` and "
            "`response.completed`. See OpenAI's "
            "[streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming) "
            "for each type.",
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model to run; loaded on first use."},
            {"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
             "A string, or a list of input items, for the model to respond to."},
            {"max_output_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens."},
            {"temperature", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Sampling temperature."},
            {"repeat_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Number between 1.0 and 2.0; 1.0 means no penalty. Higher values discourage "
             "repetition."},
            {"top_k", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of top tokens considered during sampling."},
            {"top_p", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Cumulative probability, between 0.0 and 1.0, of the top tokens considered during "
             "nucleus sampling."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Stream semantic events as they are generated. Defaults to `false`."},
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
        response.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "input": "What is the capital of France? Answer in one word.",
            "max_output_tokens": 256
        })");

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = response_event_schema();
        stream.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "input": "What is the capital of France? Answer in one word.",
            "max_output_tokens": 256,
            "stream": true
        })");

        s.responses = {response, stream};
        s.request_format = RequestFormat::Json;
        s.model_defaults_to_loaded = true;
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
