#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/omni_collection.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"
#include "lemon/streaming_proxy.h"
#include "lemon/thinking_controls.h"
#include "require_llm.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

ordered_json chat_completion_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string", "description": "Identifier of the completion."},
            "object": {"const": "chat.completion", "description": "Always `\"chat.completion\"`."},
            "created": {"type": "integer", "description": "Unix timestamp of when the completion was created."},
            "model": {"type": "string", "description": "The model that generated the response."},
            "system_fingerprint": {"type": "string", "description": "Build identifier of the backend, when it reports one."},
            "choices": {"type": "array", "description": "The generated messages, one per choice.", "items": {
                "type": "object",
                "required": ["index", "message", "finish_reason"],
                "properties": {
                    "index": {"type": "integer", "description": "Position of the choice in `choices`."},
                    "message": {
                        "type": "object",
                        "description": "The generated assistant message.",
                        "required": ["role"],
                        "properties": {
                            "role": {"const": "assistant", "description": "Always `\"assistant\"`."},
                            "content": {"type": ["string", "null"], "description": "The message text."},
                            "reasoning_content": {"type": "string", "description": "Reasoning models only: the model's thinking."},
                            "tool_calls": {"type": "array", "description": "Calls to the request's tools."}
                        }
                    },
                    "finish_reason": {"type": "string", "description": "Why generation stopped, such as `stop`, `length` or `tool_calls`."}
                }
            }},
            "usage": {
                "type": "object",
                "description": "Token counts for the request.",
                "required": ["prompt_tokens", "completion_tokens", "total_tokens"],
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
    // Router collections add their routing decision to the response.
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    return schema;
}

ordered_json chat_completion_chunk_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "title": "Chunk",
        "description": "A chunk of the assistant's message.",
        "required": ["id", "object", "created", "model", "choices"],
        "properties": {
            "id": {"type": "string", "description": "Identifier of the completion, the same on every chunk."},
            "object": {"const": "chat.completion.chunk", "description": "Always `\"chat.completion.chunk\"`."},
            "created": {"type": "integer", "description": "Unix timestamp of when the completion was created."},
            "model": {"type": "string", "description": "The backend's name for the model, such as the GGUF file path for llama.cpp models."},
            "system_fingerprint": {"type": "string", "description": "Build identifier of the backend, when it reports one."},
            "choices": {"type": "array", "description": "The next part of each choice's message.", "items": {
                "type": "object",
                "required": ["index", "delta"],
                "properties": {
                    "index": {"type": "integer", "description": "Position of the choice in `choices`."},
                    "delta": {
                        "type": "object",
                        "description": "The part of the assistant message generated since the previous chunk.",
                        "properties": {
                            "role": {"const": "assistant", "description": "First chunk only: always `\"assistant\"`."},
                            "content": {"type": ["string", "null"], "description": "The next message text."},
                            "reasoning_content": {"type": ["string", "null"], "description": "Reasoning models only: the next part of the model's thinking."},
                            "tool_calls": {"type": "array", "description": "Parts of calls to the request's tools, matched by `index`: a call's first part carries its `id` and `function.name`, and later parts carry the next piece of `function.arguments`."}
                        }
                    },
                    "finish_reason": {"type": ["string", "null"], "description": "Why generation stopped, such as `stop`, `length` or `tool_calls`, on the choice's last chunk; `null` before it."}
                }
            }},
            "usage": {"type": "object", "description": "Token counts for the request, on an extra last chunk with an empty `choices`. Sent when the request sets `stream_options.include_usage`, and always by vLLM models."},
            "timings": {"type": "object", "description": "Final chunk only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp."}
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

} // namespace

namespace openai::chat {

const RouteArg kMessages{"messages", ArgIn::JsonBody, {{"type", "array"}}, true, Support::Available,
                         "The conversation so far, in OpenAI's message format: each message has a `role` and "
                         "`content`."};
const RouteArg kLogprobs{"logprobs", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                         "`true` returns the log probability of each output token. Defaults to `false`.", {}, true};
const RouteArg kToolChoice{"tool_choice", ArgIn::JsonBody, {{"type", json::array({"string", "object"})}}, false, Support::Partial,
                           "Which tool the model must call, in OpenAI's format: `auto`, `none` or "
                           "`required`. vLLM and cloud models also take an object naming a function, "
                           "which llama.cpp models treat as `auto`.", {}, true};
const RouteArg kResponseFormat{"response_format", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
                               "The format the model must output, in OpenAI's format, such as "
                               "`{\"type\": \"json_object\"}`.", {}, true};
const RouteArg kMaxCompletionTokens{"max_completion_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                                    "OpenAI's name for `max_tokens`. Send one of the two: when both "
                                    "are sent, the backend picks, and llama.cpp uses `max_tokens` "
                                    "while vLLM uses `max_completion_tokens`.", {}, true};
const RouteArg kEnableThinking{"enable_thinking", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                               "Lemonade extension: `false` turns off reasoning on models that think. `thinking: "
                               "false` does the same, and `enable_thinking` wins when both are sent."};

} // namespace openai::chat

namespace {

using namespace openai::chat;

class ChatCompletionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.chat_completions";
        s.methods = {"POST"};
        s.paths = {"chat/completions"};
        s.prefixes = Prefixes::Quad;
        s.summary = "Generate the next message in a conversation";
        s.modality = "messages -> completion";
        s.description =
            "Generates the next assistant message for a conversation, downloading and loading the "
            "model if it is not loaded.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; see "
            "[Router API](./router.md).",
            "### Server-Side Tools\n"
            "\n"
            "Omni collection orchestration is a Lemonade extension to the Chat Completions API; "
            "requests to other models are unaffected.\n"
            "\n"
            "When `model` names an Omni **collection** model (`recipe: \"collection.omni\"`, e.g., "
            "`LMX-Omni-52B-Halo`), the route runs an internal tool-calling loop instead of a "
            "plain completion. The server injects the reference system prompt and tools, routes to "
            "the collection's chat component, executes the omni tools (image generation/editing, "
            "text-to-speech) against the matching components, and returns one OpenAI-compatible "
            "response. Generated media is embedded in the assistant `content`:\n"
            "\n"
            "- **images** → markdown `![generated image](data:image/png;base64,…)`\n"
            "- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`\n"
            "\n"
            "Both non-streaming and `stream: true` are supported. In streaming mode the media "
            "arrives in `content` deltas of at most 16,000 characters each, the moment its tool "
            "finishes, and an error arrives as `Error: <message>` text in `content` followed by "
            "`finish_reason: \"stop\"`.\n"
            "\n"
            "**Merge semantics.** The built-in omni system prompt goes before a client-provided "
            "one. Client-provided `tools` are merged with the built-in omni tools. The "
            "server resolves omni tool calls internally; calls to client-provided tools are "
            "returned in a `finish_reason: \"tool_calls\"` response for the client to execute and "
            "resume. Targeting a collection name invokes the server-side loop; targeting a "
            "component LLM name bypasses it and returns a plain completion. See [Lemonade Omni "
            "Models](../dev/lemonade-omni.md) for details.",
        };
        s.args = {
            kMessages, model_loading::kModel, common::kStream, openai::text::kStop, kLogprobs,
            generation::kTemperature, openai::text::kRepeatPenalty, openai::text::kFrequencyPenalty,
            openai::text::kSeed, generation::kTopK, generation::kTopP, generation::kTools,
            kToolChoice, kResponseFormat, openai::text::kMaxTokens, kMaxCompletionTokens,
            kEnableThinking, openai::text::kRouteTrace, model_loading::kCtxSize
        };

        RouteResponse completion;
        completion.format = ResponseFormat::Json;
        completion.schema = chat_completion_schema();

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = {{"description", "One event per `data:` line. The stream ends with `data: [DONE]`."}, {"oneOf", ordered_json::array({
            chat_completion_chunk_schema(), StreamingProxy::error_event_schema()})}};

        s.responses = {completion, stream};

        RouteExample completion_example;
        completion_example.name = "non-streaming";
        completion_example.format = ResponseFormat::Json;
        completion_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the population of Paris?"}],
            "stream": false
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::EventStream;
        stream_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the population of Paris?"}],
            "stream": true
        })");

        RouteExample image_example;
        image_example.name = "image input";
        image_example.description =
            "Asks a vision model what `image.png` shows. A `messages[*].content` array mixes `text` "
            "and `image_url` items, with each image as a base64 data URL, such as one from "
            "`FileReader.readAsDataURL(...)` in a web app.";
        image_example.format = ResponseFormat::Json;
        image_example.args = ordered_json::parse(R"({
            "model": "Qwen3.5-0.8B-GGUF",
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "What is in this image?"},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64,@fixtures/image.png"}}
            ]}],
            "stream": false
        })");

        RouteExample tools_example;
        tools_example.name = "server-side tools";
        tools_example.description =
            "Runs the [server-side tools](#server-side-tools) loop on `LMX-Omni-5.5B-Lite`, whose "
            "image tool draws the requested picture.";
        tools_example.format = ResponseFormat::Json;
        tools_example.args = ordered_json::parse(R"({
            "model": "LMX-Omni-5.5B-Lite",
            "messages": [{"role": "user", "content": "Draw a red apple on a table."}],
            "stream": false
        })");

        s.examples = {completion_example, stream_example, image_example, tools_example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNotLlm, kBackendError,
                                 kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        // Router policies and Omni collections call backends here, before run() does.
        auto cancel = cancel_on_disconnect(req);
        if (req.has(generation::kTools)) {
            LOG(DEBUG, "Server") << "Tools present in request: " << req.arg(generation::kTools).size() << " tool(s)" << std::endl;
            LOG(DEBUG, "Server") << "Tools JSON: " << req.arg(generation::kTools).dump() << std::endl;
        } else {
            LOG(DEBUG, "Server") << "No tools in request" << std::endl;
        }

        // An Omni collection has no backend of its own: its orchestrator loads each
        // component, so it answers before ModelRoute's auto-load.
        if (is_omni_collection(ctx_, req.model)) {
            run_omni_collection(ctx_, req, res);
            return false;
        }
        req.route_decision = router_dispatch(ctx_, req.model, req.value(openai::text::kRouteTrace, false),
                                             req.body);
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        if (!require_llm(*ctx_.router, req.model, "chat completion", res)) {
            return;
        }

        bool is_streaming = req.value(common::kStream, false);

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
            set_error_response(response, res, kBackendError);
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
