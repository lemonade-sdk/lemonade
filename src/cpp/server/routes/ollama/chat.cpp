#include <string>
#include <unordered_map>

#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

// Ollama sends tool-call arguments as an object; OpenAI sends a JSON string.
json normalize_ollama_tool_calls(json tool_calls) {
    if (!tool_calls.is_array()) {
        return tool_calls;
    }

    for (auto& tool_call : tool_calls) {
        if (!tool_call.is_object() ||
            !tool_call.contains("function") ||
            !tool_call["function"].is_object()) {
            continue;
        }

        auto& function = tool_call["function"];
        if (!function.contains("arguments") || !function["arguments"].is_string()) {
            continue;
        }

        std::string arguments = function["arguments"].get<std::string>();
        try {
            function["arguments"] = json::parse(arguments);
        } catch (const std::exception&) {
            function["arguments"] = json::object();
        }
    }

    return tool_calls;
}

json normalize_openai_tool_calls(json tool_calls) {
    if (!tool_calls.is_array()) {
        return tool_calls;
    }

    for (auto& tool_call : tool_calls) {
        if (!tool_call.is_object() ||
            !tool_call.contains("function") ||
            !tool_call["function"].is_object()) {
            continue;
        }

        if (!tool_call.contains("type") || !tool_call["type"].is_string()) {
            tool_call["type"] = "function";
        }

        auto& function = tool_call["function"];
        if (!function.contains("arguments") || function["arguments"].is_string()) {
            continue;
        }

        function["arguments"] = function["arguments"].dump();
    }

    return tool_calls;
}

const RouteArg kMessages{"messages", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
                         "Conversation so far. Each message has a `role` and `content`, plus optional "
                         "`images` (base64 strings), `tool_calls`, and, on `tool` messages, `tool_name` or "
                         "`tool_call_id`."};
const RouteArg kOptions{"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Partial,
                        "Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum "
                        "generated tokens) and `repeat_penalty`, which is sent as OpenAI's "
                        "`frequency_penalty`. These keys are also accepted at the top level, where they win. "
                        "`num_ctx` sets the context size when this request loads the model, unless "
                        "`ctx_size` is sent. Other keys are ignored."};
const RouteArg kFormat{"format", ArgIn::JsonBody, {{"type", json::array({"string", "object"})}}, false,
                       Support::Partial,
                       "`json` constrains the reply to a JSON object. A JSON schema is accepted but not "
                       "applied."};
const RouteArg kThink{"think", ArgIn::JsonBody,
                      {{"type", json::array({"boolean", "string"})},
                       {"enum", json::array({true, false, "low", "medium", "high", "max"})}},
                      false, Support::NotAvailable,
                      "`true`, `false`, `low`, `medium`, `high` or `max`. Not applied: reasoning models "
                      "think regardless. Qwen3 models skip their reasoning when the prompt ends with "
                      "`/no_think`."};

json convert_ollama_to_openai_chat(const RouteRequest& req) {
    json openai_req;

    std::string model = strip_latest_tag(req.value(gateway::kModel, ""));
    openai_req[model_loading::kModel.name] = model;

    if (req.has(kMessages)) {
        json messages = json::array();
        std::unordered_map<std::string, std::string> tool_call_ids_by_name;
        for (const auto& msg : req.arg(kMessages)) {
            json openai_msg;
            std::string role = msg.value("role", "user");
            openai_msg["role"] = role;

            // Ollama carries images beside the text; OpenAI wants one content array.
            if (msg.contains("images") && msg["images"].is_array() && !msg["images"].empty()) {
                json content_parts = json::array();
                if (msg.contains("content") && !msg["content"].get<std::string>().empty()) {
                    content_parts.push_back({{"type", "text"}, {"text", msg["content"]}});
                }
                for (const auto& img : msg["images"]) {
                    content_parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", "data:image/png;base64," + img.get<std::string>()}}}
                    });
                }
                openai_msg["content"] = content_parts;
            } else {
                openai_msg["content"] = msg.value("content", "");
            }

            if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
                auto tool_calls = normalize_openai_tool_calls(msg["tool_calls"]);
                openai_msg["tool_calls"] = tool_calls;
                for (const auto& tool_call : tool_calls) {
                    if (tool_call.contains("id") && tool_call["id"].is_string() &&
                        tool_call.contains("function") && tool_call["function"].is_object() &&
                        tool_call["function"].contains("name") && tool_call["function"]["name"].is_string()) {
                        tool_call_ids_by_name[tool_call["function"]["name"].get<std::string>()] =
                            tool_call["id"].get<std::string>();
                    }
                }
            }

            // Ollama names the tool a result answers; OpenAI needs the call's id.
            if (role == "tool") {
                if (msg.contains("tool_call_id") && msg["tool_call_id"].is_string()) {
                    openai_msg["tool_call_id"] = msg["tool_call_id"];
                } else if (msg.contains("tool_name") && msg["tool_name"].is_string()) {
                    auto it = tool_call_ids_by_name.find(msg["tool_name"].get<std::string>());
                    if (it != tool_call_ids_by_name.end()) {
                        openai_msg["tool_call_id"] = it->second;
                    }
                }
            }

            messages.push_back(openai_msg);
        }
        openai_req[openai::chat::kMessages.name] = messages;
    }

    map_ollama_options(req.arg(kOptions), req.body, openai_req);

    if (req.has(generation::kTools)) {
        openai_req[generation::kTools.name] = req.arg(generation::kTools);
    }

    if (req.has(kFormat) && req.arg(kFormat).is_string() &&
        req.arg(kFormat).get<std::string>() == "json") {
        openai_req[openai::chat::kResponseFormat.name] = {{"type", "json_object"}};
    }

    if (req.has(kThink)) {
        openai_req[openai::chat::kEnableThinking.name] = req.arg(kThink);
    }

    // The caller decides whether to stream.
    openai_req[common::kStream.name] = false;

    return openai_req;
}

json convert_openai_chat_to_ollama(const json& openai_response, const std::string& model) {
    json ollama_res;
    ollama_res["model"] = model;
    ollama_res["created_at"] = "2024-01-01T00:00:00Z";

    if (openai_response.contains("choices") && !openai_response["choices"].empty()) {
        const auto& choice = openai_response["choices"][0];
        if (choice.contains("message")) {
            const auto& message = choice["message"];
            json msg;
            msg["role"] = (message.contains("role") && message["role"].is_string())
                          ? message["role"].get<std::string>() : "assistant";
            msg["content"] = (message.contains("content") && message["content"].is_string())
                             ? message["content"].get<std::string>() : "";

            // Ollama reports reasoning as "thinking".
            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                msg["thinking"] = message["reasoning_content"].get<std::string>();
            }

            if (message.contains("tool_calls")) {
                msg["tool_calls"] = normalize_ollama_tool_calls(message["tool_calls"]);
            }

            ollama_res["message"] = msg;
        }

        ollama_res["done_reason"] = (choice.contains("finish_reason") && choice["finish_reason"].is_string())
                                    ? choice["finish_reason"].get<std::string>() : "stop";
    }

    ollama_res["done"] = true;

    // OpenAI usage carries no timings, so the durations stay 0 unless a llama.cpp
    // backend reports timings below.
    if (openai_response.contains("usage")) {
        const auto& usage = openai_response["usage"];
        int prompt_tokens = usage.value("prompt_tokens", 0);
        int completion_tokens = usage.value("completion_tokens", 0);

        ollama_res["prompt_eval_count"] = prompt_tokens;
        ollama_res["eval_count"] = completion_tokens;

        ollama_res["total_duration"] = 0;
        ollama_res["load_duration"] = 0;
        ollama_res["prompt_eval_duration"] = 0;
        ollama_res["eval_duration"] = 0;
    }

    if (openai_response.contains("timings")) {
        const auto& timings = openai_response["timings"];
        if (timings.contains("prompt_n"))
            ollama_res["prompt_eval_count"] = timings["prompt_n"];
        if (timings.contains("predicted_n"))
            ollama_res["eval_count"] = timings["predicted_n"];
        if (timings.contains("prompt_ms"))
            ollama_res["prompt_eval_duration"] = static_cast<int64_t>(timings["prompt_ms"].get<double>() * 1000000);
        if (timings.contains("predicted_ms"))
            ollama_res["eval_duration"] = static_cast<int64_t>(timings["predicted_ms"].get<double>() * 1000000);
    }

    return ollama_res;
}

json convert_openai_delta_to_ollama(const json& openai_chunk, const std::string& model) {
    json ollama_chunk;
    ollama_chunk["model"] = model;
    ollama_chunk["created_at"] = "2024-01-01T00:00:00Z";
    ollama_chunk["done"] = false;

    if (openai_chunk.contains("choices") && !openai_chunk["choices"].empty()) {
        const auto& choice = openai_chunk["choices"][0];
        if (choice.contains("delta")) {
            const auto& delta = choice["delta"];
            json msg;
            msg["role"] = (delta.contains("role") && delta["role"].is_string())
                          ? delta["role"].get<std::string>() : "assistant";
            msg["content"] = (delta.contains("content") && delta["content"].is_string())
                             ? delta["content"].get<std::string>() : "";

            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                msg["thinking"] = delta["reasoning_content"].get<std::string>();
            }

            if (delta.contains("tool_calls")) {
                msg["tool_calls"] = delta["tool_calls"];
            }

            ollama_chunk["message"] = msg;
        }

        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            ollama_chunk["done"] = true;
            ollama_chunk["done_reason"] = choice["finish_reason"];
        }
    }

    return ollama_chunk;
}

ordered_json chat_response_schema(bool line) {
    ordered_json schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["model", "created_at", "done"],
        "properties": {
            "model": {"type": "string", "description": "The model that generated the response."},
            "created_at": {"type": "string", "description": "Always `2024-01-01T00:00:00Z`."},
            "message": {
                "type": "object",
                "description": "The assistant's message; in a streamed line, the next piece of it.",
                "required": ["role", "content"],
                "properties": {
                    "role": {"type": "string", "description": "Always `assistant`."},
                    "content": {"type": "string", "description": "The message text."},
                    "thinking": {"type": "string", "description": "Reasoning models only: the model's thinking."},
                    "tool_calls": {"type": "array", "description": "Calls to the request's tools, with arguments as objects."}
                }
            },
            "done": {"type": "boolean", "description": "`false` while the message streams; `true` once it ends, and on a response that is not streamed."},
            "done_reason": {"type": "string", "description": "`stop`, `length`, `tool_calls`, or `unload` after an unload request."},
            "prompt_eval_count": {"type": "integer", "description": "Prompt tokens."},
            "eval_count": {"type": "integer", "description": "Generated tokens."},
            "total_duration": {"type": "integer", "description": "Always `0`."},
            "load_duration": {"type": "integer", "description": "Always `0`."},
            "prompt_eval_duration": {"type": "integer", "description": "Prompt processing time in nanoseconds, when the backend reports timings; otherwise `0`."},
            "eval_duration": {"type": "integer", "description": "Generation time in nanoseconds, when the backend reports timings; otherwise `0`."}
        }
    })");
    if (line) {
        schema["description"] = "Each line carries the next message piece with `done: false`. When "
                                "the message ends, a line with `done: true` and the real "
                                "`done_reason` follows, then a last `done: true` line with the token "
                                "counts, which are `0` when the backend reports no usage, and "
                                "`done_reason` `stop`.";
        schema["properties"]["prompt_eval_duration"]["description"] =
            "Prompt processing time in nanoseconds on a request with tools, when the backend "
            "reports timings; otherwise `0`.";
        schema["properties"]["eval_duration"]["description"] =
            "Generation time in nanoseconds on a request with tools, when the backend reports "
            "timings; otherwise `0`.";
    }
    return schema;
}

class ChatRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.chat";
        s.methods = {"POST"};
        s.paths = {"/api/chat"};
        s.prefixes = Prefixes::Root;
        s.summary = "Generate the next chat message";
        s.description =
            "Generates the next assistant message for a conversation, loading the model on "
            "first use. Lemonade runs it as an OpenAI chat completion and converts the result.";
        s.notes = {
            "A request with tools and streaming runs non-streaming on the backend and answers "
            "with two NDJSON lines: the whole message, then the final line.",
        };
        s.args = {
            gateway::kModel, kMessages, ollama::kStream, kOptions, generation::kTools, kFormat,
            kThink, ollama::kKeepAlive, model_loading::kCtxSize
        };

        RouteResponse full;
        full.format = ResponseFormat::Json;
        full.schema = chat_response_schema(false);

        RouteResponse stream;
        stream.format = ResponseFormat::JsonLines;
        stream.schema = chat_response_schema(true);

        s.responses = {full, stream};

        RouteExample full_example;
        full_example.name = "non-streaming";
        full_example.format = ResponseFormat::Json;
        full_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
            "stream": false
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::JsonLines;
        stream_example.args = ordered_json::parse(R"({
            "model": "Qwen3-0.6B-GGUF",
            "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
            "stream": true
        })");

        s.examples = {full_example, stream_example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid}, gateway_load_errors(),
                                {kOllamaBackendError, ApiRoute::kUnexpectedException}});
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string model = strip_latest_tag(req.arg(gateway::kModel).get<std::string>());

            auto messages = req.value(kMessages, json::array());
            if (messages.empty() && req.has(ollama::kKeepAlive) &&
                req.arg(ollama::kKeepAlive) == 0) {
                LOG(INFO, "OllamaApi") << "POST /api/chat - Unloading model: " << model << std::endl;
                try {
                    ctx_.router->unload_model(model);
                } catch (...) {
                    // Unloading a model that is not loaded is not an error here.
                }
                json ollama_res = {
                    {"model", model},
                    {"created_at", "2024-01-01T00:00:00Z"},
                    {"message", {{"role", "assistant"}, {"content", ""}}},
                    {"done", true},
                    {"done_reason", "unload"}
                };
                res.set_content(ollama_res.dump(), "application/json");
                return;
            }

            if (!gateway_ensure_loaded(*ctx_.model_loader, model,
                                       gateway_load_options(req.arg(kOptions), req.arg(model_loading::kCtxSize)),
                                       ErrorFormat::Plain, res)) {
                return;
            }

            bool stream = req.value(ollama::kStream, true);

            auto openai_req = convert_ollama_to_openai_chat(req);

            bool has_tools = req.has(generation::kTools) &&
                             req.arg(generation::kTools).is_array() &&
                             !req.arg(generation::kTools).empty();

            if (stream && has_tools) {
                LOG(INFO, "OllamaApi") << "POST /api/chat - Streaming requested with tools; using non-streaming backend call (model: " << model << ")" << std::endl;

                auto openai_response = ctx_.router->chat_completion(openai_req);

                if (write_ollama_backend_error(openai_response, res)) return;

                auto ollama_response = convert_openai_chat_to_ollama(openai_response, model);
                json done_response = {
                    {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                    {"message", {{"role", "assistant"}, {"content", ""}}},
                    {"done", true},
                    {"done_reason", ollama_response.value("done_reason", "stop")},
                    {"total_duration", ollama_response.value("total_duration", 0)},
                    {"load_duration", ollama_response.value("load_duration", 0)},
                    {"prompt_eval_count", ollama_response.value("prompt_eval_count", 0)},
                    {"prompt_eval_duration", ollama_response.value("prompt_eval_duration", 0)},
                    {"eval_count", ollama_response.value("eval_count", 0)},
                    {"eval_duration", ollama_response.value("eval_duration", 0)}
                };

                ollama_response["done"] = false;
                std::string body = ollama_response.dump() + "\n" + done_response.dump() + "\n";
                res.set_content(body, "application/x-ndjson");
            } else if (stream) {
                LOG(INFO, "OllamaApi") << "POST /api/chat - Streaming (model: " << model << ")" << std::endl;

                openai_req[common::kStream.name] = true;
                req.body = openai_req;
                Router* router = ctx_.router;
                stream_response(req, res, [router, model](const std::string& body, httplib::DataSink& sink) {
                    stream_sse_to_ndjson(body, sink,
                        [&model](const json& chunk) {
                            return convert_openai_delta_to_ollama(chunk, model);
                        },
                        [&model](int prompt_eval_count, int eval_count) -> json {
                            return {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"message", {{"role", "assistant"}, {"content", ""}}},
                                {"done", true}, {"done_reason", "stop"},
                                {"total_duration", 0}, {"load_duration", 0},
                                {"prompt_eval_count", prompt_eval_count}, {"prompt_eval_duration", 0},
                                {"eval_count", eval_count}, {"eval_duration", 0}
                            };
                        },
                        [router](const std::string& openai_body, httplib::DataSink& s) {
                            router->chat_completion_stream(openai_body, s);
                        });
                }, "application/x-ndjson");
            } else {
                LOG(INFO, "OllamaApi") << "POST /api/chat - Non-streaming (model: " << model << ")" << std::endl;

                auto openai_response = ctx_.router->chat_completion(openai_req);

                if (write_ollama_backend_error(openai_response, res)) return;

                auto ollama_response = convert_openai_chat_to_ollama(openai_response, model);
                res.set_content(ollama_response.dump(), "application/json");
            }

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/chat: " << e.what() << std::endl;
            write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_chat_route(ServerContext& ctx) {
    return std::make_unique<ChatRoute>(ctx);
}

} // namespace lemon
