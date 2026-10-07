#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/route_decision_response.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/omni_collection.h"
#include "lemon/thinking_controls.h"

namespace lemon {
namespace {

using json = nlohmann::json;

json chat_completion_schema() {
    auto schema = json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string"},
            "object": {"const": "chat.completion"},
            "created": {"type": "integer"},
            "model": {"type": "string"},
            "choices": {"type": "array", "items": {
                "type": "object",
                "required": ["index", "message", "finish_reason"],
                "properties": {
                    "index": {"type": "integer"},
                    "message": {
                        "type": "object",
                        "required": ["role"],
                        "properties": {
                            "role": {"const": "assistant"},
                            "content": {"type": ["string", "null"]},
                            "reasoning_content": {"type": "string", "description": "Reasoning models only: the model's thinking."},
                            "tool_calls": {"type": "array", "description": "Calls to the request's tools."}
                        }
                    },
                    "finish_reason": {"type": "string"}
                }
            }},
            "usage": {
                "type": "object",
                "required": ["prompt_tokens", "completion_tokens", "total_tokens"],
                "properties": {
                    "prompt_tokens": {"type": "integer"},
                    "completion_tokens": {"type": "integer"},
                    "total_tokens": {"type": "integer"}
                }
            }
        }
    })");
    // Router collections add their routing decision to the response.
    schema["properties"]["x_lemonade_route"] = route_decision_schema();
    return schema;
}

json chat_completion_chunk_schema() {
    auto schema = json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string"},
            "object": {"const": "chat.completion.chunk"},
            "created": {"type": "integer"},
            "model": {"type": "string"},
            "choices": {"type": "array", "items": {
                "type": "object",
                "required": ["index", "delta"],
                "properties": {
                    "index": {"type": "integer"},
                    "delta": {
                        "type": "object",
                        "properties": {
                            "role": {"const": "assistant"},
                            "content": {"type": ["string", "null"]},
                            "reasoning_content": {"type": ["string", "null"]}
                        }
                    },
                    "finish_reason": {"type": ["string", "null"]}
                }
            }},
            "usage": {"type": "object", "description": "Final chunk only: token usage."}
        }
    })");
    schema["properties"]["x_lemonade_route"] = route_decision_schema();
    return schema;
}

class ChatCompletionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.chat_completions";
        s.methods = {"POST"};
        s.paths = {"chat/completions"};
        s.prefixes = Prefixes::Quad;
        s.summary = "Chat Completions";
        s.description =
            "Generates the next assistant message for a conversation, loading the model on "
            "first use.";
        s.notes = {
            "Naming an Omni collection (`recipe: \"collection.omni\"`) runs a server-side "
            "tool-calling loop instead; see [Server-Side Tools](#server-side-tools). Naming a "
            "`collection.router` model routes the request to one of its candidates; see "
            "[Router API](./router.md).",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model to run; loaded on first use."},
            {"messages", ArgIn::JsonBody, {{"type", "array"}}, true, Support::Available,
             "Conversation so far. Each message has a `role` (`system`, `user`, `assistant` or "
             "`tool`) and `content`; see [Image Input](#image-input) for images."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Stream tokens as server-sent events as they are generated. Defaults to `false`."},
            {"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
             "A string or an array of up to 4 strings where generation stops. The returned text "
             "does not contain the stop sequence."},
            {"logprobs", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::NotAvailable,
             "Return the log probability of each output token."},
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
            {"tools", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "Tools the model may call."},
            {"max_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens. Deprecated by OpenAI in favor of "
             "`max_completion_tokens`; the two are mutually exclusive."},
            {"max_completion_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens. Mutually exclusive with `max_tokens`."},
            {"enable_thinking", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension: `false` turns off reasoning on models that think. The "
             "OpenAI-compatible `thinking: false` is accepted too."},
            {"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "Lemonade extension for `collection.router` models: `true` adds the routing "
             "decision to the response as `x_lemonade_route`."},
            {"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Lemonade extension: context size to load the model with, when this request "
             "loads it. Ignored when the model is already loaded."},
        };

        RouteResponse completion;
        completion.format = ResponseFormat::Json;
        completion.schema = chat_completion_schema();
        completion.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the capital of France?"}],
            "enable_thinking": false
        })");

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = chat_completion_chunk_schema();
        stream.example = json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the capital of France?"}],
            "enable_thinking": false,
            "stream": true
        })");

        s.responses = {completion, stream};
        s.request_format = RequestFormat::Json;
        s.model_defaults_to_loaded = true;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        // Router policies and Omni collections call backends here, before run() does.
        auto cancel = cancel_on_disconnect(req);
        if (req.body.contains("tools")) {
            LOG(DEBUG, "Server") << "Tools present in request: " << req.body["tools"].size() << " tool(s)" << std::endl;
            LOG(DEBUG, "Server") << "Tools JSON: " << req.body["tools"].dump() << std::endl;
        } else {
            LOG(DEBUG, "Server") << "No tools in request" << std::endl;
        }

        // An Omni collection has no backend of its own: its orchestrator loads each
        // component, so it answers before ModelRoute's auto-load.
        if (is_omni_collection(ctx_, req.model)) {
            run_omni_collection(ctx_, req, res);
            return false;
        }
        req.route_decision = router_dispatch(ctx_, req);
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        if (ctx_.router->get_model_type(req.model) != ModelType::LLM) {
            LOG(ERROR, "Server") << "Model does not support chat completion" << std::endl;
            write_openai_error(res, 400, "This model does not support chat completion. Only LLM models support this endpoint.");
            return;
        }

        bool is_streaming = req.body.contains("stream") && req.body["stream"].get<bool>();

        // OpenCode and other OpenAI-compatible clients may send thinking=false
        // instead of Lemonade's enable_thinking=false.
        normalize_thinking_controls(req.body);

        if (is_streaming) {
            LOG(INFO, "Server") << "POST /api/v1/chat/completions - Streaming" << std::endl;
            Router* router = ctx_.router;
            stream_response(req, res, [router](const std::string& body, httplib::DataSink& sink) {
                router->chat_completion_stream(body, sink);
            });
            return;
        }

        LOG(INFO, "Server") << "POST /api/v1/chat/completions - 200 OK" << std::endl;
        auto cancel = cancel_on_disconnect(req);
        auto response = ctx_.router->chat_completion(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Backend returned error response: " << response["error"].dump() << std::endl;
            set_error_response(response, res);
            return;
        }

        attach_route_decision(response, res, req.route_decision);
        if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
            auto& first_choice = response["choices"][0];
            if (first_choice.contains("message")) {
                auto& message = first_choice["message"];
                if (message.contains("tool_calls")) {
                    LOG(DEBUG, "Server") << "Response contains tool_calls: " << message["tool_calls"].dump() << std::endl;
                } else {
                    LOG(DEBUG, "Server") << "Response message does NOT contain tool_calls" << std::endl;
                    if (message.contains("content")) {
                        LOG(DEBUG, "Server") << "Message content: " << message["content"].get<std::string>().substr(0, 200) << std::endl;
                    }
                }
            }
        }

        res.set_content(response.dump(), "application/json");
        record_usage(response, req);
    }

    LoadSpan load_span() const override { return {"LLM", "chat.completions"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_chat_completions_route(ServerContext& ctx) {
    return std::make_unique<ChatCompletionsRoute>(ctx);
}

} // namespace lemon
