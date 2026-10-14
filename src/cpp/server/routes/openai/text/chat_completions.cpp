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
using ordered_json = nlohmann::ordered_json;

ordered_json chat_completion_schema() {
    auto schema = ordered_json::parse(R"({
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

ordered_json chat_completion_chunk_schema() {
    auto schema = ordered_json::parse(R"({
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
        s.modality = "messages -> completion";
        s.description =
            "Generates the next assistant message for a conversation, loading the model on first "
            "use.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; see "
            "[Router API](./router.md).",
            "### Image understanding input format (OpenAI-compatible)\n"
            "\n"
            "To send images to `chat/completions`, pass a `messages[*].content` array that mixes "
            "`text` and `image_url` items. The image can be provided as a base64 data URL (for "
            "example, from `FileReader.readAsDataURL(...)` in web apps).\n"
            "\n"
            "#### Example request\n"
            "\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/v1/chat/completions \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "        \"model\": \"Qwen2.5-VL-7B-Instruct\",\n"
            "        \"messages\": [\n"
            "          {\n"
            "            \"role\": \"user\",\n"
            "            \"content\": [\n"
            "              {\"type\": \"text\", \"text\": \"What is in this image?\"},\n"
            "              {\"type\": \"image_url\", \"image_url\": {\"url\": \"data:image/jpeg;base64,/9j/4AAQSkZJRgABAQAAAQABAAD...\"}}\n"
            "            ]\n"
            "          }\n"
            "        ],\n"
            "        \"stream\": false\n"
            "      }'\n"
            "```\n"
            "\n"
            "#### Example response\n"
            "\n"
            "```json\n"
            "{\n"
            "  \"id\": \"0\",\n"
            "  \"object\": \"chat.completion\",\n"
            "  \"created\": 1742927481,\n"
            "  \"model\": \"Qwen2.5-VL-7B-Instruct\",\n"
            "  \"choices\": [{\n"
            "    \"index\": 0,\n"
            "    \"message\": {\n"
            "      \"role\": \"assistant\",\n"
            "      \"content\": \"The image shows a red apple resting on a wooden table.\"\n"
            "    },\n"
            "    \"finish_reason\": \"stop\"\n"
            "  }]\n"
            "}\n"
            "```",
            "### Server-side tools\n"
            "\n"
            "Omni collection orchestration is a Lemonade extension to the Chat Completions API; "
            "requests to other models are unaffected.\n"
            "\n"
            "When `model` names an Omni **collection** model (`recipe: \"collection.omni\"`, e.g., "
            "`LMX-Omni-52B-Halo`), this endpoint runs an internal tool-calling loop instead of a "
            "plain completion. The server injects the reference system prompt and tools, routes to "
            "the collection's chat component, executes the omni tools (image generation/editing, "
            "text-to-speech) against the matching components, and returns one OpenAI-compatible "
            "response. Generated media is embedded in the assistant `content`:\n"
            "\n"
            "- **images** → markdown `![generated image](data:image/png;base64,…)`\n"
            "- **speech** → `<audio>data:audio/mpeg;base64,…</audio>`\n"
            "\n"
            "Both non-streaming and `stream: true` are supported. In streaming mode the media "
            "arrives as a content delta on a `chat.completion.chunk` frame the moment its tool "
            "finishes.\n"
            "\n"
            "**Merge semantics.** The built-in omni system prompt goes before a client-provided "
            "one. Client-provided `tools` are merged with the built-in omni tools. The "
            "server resolves omni tool calls internally; calls to client-provided tools are "
            "returned in a `finish_reason: \"tool_calls\"` response for the client to execute and "
            "resume. Targeting a collection name invokes the server-side loop; targeting a "
            "component LLM name bypasses it and returns a plain completion. See [Lemonade Omni "
            "Models](../dev/lemonade-omni.md) for details.\n"
            "\n"
            "#### Example request\n"
            "\n"
            "```bash\n"
            "curl -X POST http://localhost:13305/v1/chat/completions \\\n"
            "  -H \"Content-Type: application/json\" \\\n"
            "  -d '{\n"
            "        \"model\": \"LMX-Omni-52B-Halo\",\n"
            "        \"messages\": [{\"role\": \"user\", \"content\": \"Draw a red apple on a table.\"}],\n"
            "        \"stream\": false\n"
            "      }'\n"
            "```\n"
            "\n"
            "#### Example response\n"
            "\n"
            "The image tool runs during the loop and its output is embedded as a markdown image in "
            "the assistant `content`:\n"
            "\n"
            "```json\n"
            "{\n"
            "  \"id\": \"0\",\n"
            "  \"object\": \"chat.completion\",\n"
            "  \"created\": 1742927481,\n"
            "  \"model\": \"LMX-Omni-52B-Halo\",\n"
            "  \"choices\": [{\n"
            "    \"index\": 0,\n"
            "    \"message\": {\n"
            "      \"role\": \"assistant\",\n"
            "      \"content\": \"Here is a red apple on a table.\\n\\n![generated image](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAA...)\"\n"
            "    },\n"
            "    \"finish_reason\": \"stop\"\n"
            "  }]\n"
            "}\n"
            "```",
        };
        s.args = {
            {"messages", ArgIn::JsonBody, {{"type", "array"}}, true, Support::Available,
             "The conversation so far, in OpenAI's message format: each message has a `role` and "
             "`content`."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The model to use for the completion."},
            {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If true, tokens will be sent as they are generated. If false, the response will be "
             "sent as a single message once complete. Defaults to false."},
            {"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
             "Up to 4 sequences where the API will stop generating further tokens. The returned "
             "text will not contain the stop sequence. Can be a string or an array of strings."},
            {"logprobs", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "If true, returns the log probability of each output token. Defaults to false."},
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
            {"tools", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
             "A list of tools the model may call."},
            {"max_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Upper bound on generated tokens. Wins over `max_completion_tokens` when both are "
             "sent."},
            {"max_completion_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "OpenAI's name for `max_tokens`."},
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

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = chat_completion_chunk_schema();

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

        s.examples = {completion_example, stream_example};
        s.request_format = RequestFormat::Json;
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
