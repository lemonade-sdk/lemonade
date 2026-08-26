#include "lemon/anthropic_error.h"
#include "lemon/anthropic_relay_headers.h"
#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/error_types.h"
#include "lemon/ollama_api.h"
#include "lemon/utils/http_client.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace lemon {

namespace {

static void add_warning(std::vector<std::string>& warnings, const std::string& warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

static std::string join_strings(const std::vector<std::string>& parts, const char* sep = "\n") {
    std::ostringstream os;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) os << sep;
        os << parts[i];
    }
    return os.str();
}

static std::string join_text_blocks(const json& value, std::vector<std::string>& warnings, const std::string& field_name) {
    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (!value.is_array()) {
        add_warning(warnings, "Ignored non-string/non-array '" + field_name + "' field");
        return "";
    }

    std::vector<std::string> parts;
    for (const auto& block : value) {
        if (!block.is_object()) {
            add_warning(warnings, "Ignored non-object block in '" + field_name + "'");
            continue;
        }

        std::string type = block.value("type", "");
        if (type == "text" && block.contains("text") && block["text"].is_string()) {
            parts.push_back(block["text"].get<std::string>());
            continue;
        }

        add_warning(warnings, "Ignored unsupported '" + field_name + "' block type: " + type);
    }

    return join_strings(parts);
}

static std::string map_finish_reason_to_anthropic_stop_reason(const json& choice) {
    std::string finish_reason = choice.value("finish_reason", "stop");

    if (finish_reason == "length") {
        return "max_tokens";
    }
    if (finish_reason == "tool_calls") {
        return "tool_use";
    }
    return "end_turn";
}

static std::string generate_anthropic_message_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "msg_" + std::to_string(millis);
}

static bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string payload = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(payload.c_str(), payload.size());
}

static std::string stringify_anthropic_tool_result_content(const json& content,
                                                           std::vector<std::string>& warnings) {
    if (content.is_string()) {
        return content.get<std::string>();
    }

    if (content.is_array()) {
        std::vector<std::string> parts;
        for (const auto& block : content) {
            if (!block.is_object()) {
                add_warning(warnings, "Ignored non-object block in tool_result.content");
                continue;
            }

            std::string type = block.value("type", "");
            if (type == "text" && block.contains("text") && block["text"].is_string()) {
                parts.push_back(block["text"].get<std::string>());
                continue;
            }

            add_warning(warnings, "Ignored unsupported block type in tool_result.content: " + type);
        }

        return join_strings(parts);
    }

    if (content.is_object()) {
        return content.dump();
    }

    add_warning(warnings, "Ignored unsupported type for tool_result.content");
    return "";
}

static json parse_openai_tool_arguments(const json& tool_call, std::vector<std::string>& warnings) {
    if (!tool_call.is_object() || !tool_call.contains("function") || !tool_call["function"].is_object()) {
        return json::object();
    }

    const auto& fn = tool_call["function"];
    if (!fn.contains("arguments")) {
        return json::object();
    }

    if (fn["arguments"].is_object()) {
        return fn["arguments"];
    }

    if (fn["arguments"].is_string()) {
        const std::string args_str = fn["arguments"].get<std::string>();
        if (args_str.empty()) {
            return json::object();
        }

        try {
            auto parsed = json::parse(args_str);
            if (parsed.is_object()) {
                return parsed;
            }
            add_warning(warnings, "Tool arguments were not an object; wrapped as _value");
            return json{{"_value", parsed}};
        } catch (...) {
            add_warning(warnings, "Failed to parse tool arguments as JSON; wrapped as _raw");
            return json{{"_raw", args_str}};
        }
    }

    add_warning(warnings, "Tool arguments had unsupported type; using empty object");
    return json::object();
}

static bool set_anthropic_backend_error_response(const json& response, httplib::Response& res) {
    if (!response.contains("error")) {
        return false;
    }

    const auto& error = response["error"];
    std::cerr << "[OllamaApi] Backend returned error: " << error.dump() << std::endl;

    const int status = anthropic::backend_error_http_status(error);
    res.status = status;
    res.set_content(anthropic::build_anthropic_error(error, status).dump(), "application/json");
    return true;
}

static void set_anthropic_residency_conflict_response(
    const RouterResidencyConflictException& error,
    httplib::Response& res) {
    res.status = 409;
    json body = {
        {"type", "error"},
        {"error", {
            {"type", ErrorType::ROUTER_RESIDENCY_CONFLICT},
            {"message", error.what()},
        }},
    };
    res.set_content(body.dump(), "application/json");
}

// Everything needed to relay one /v1/messages request to a provider that
// speaks the Anthropic Messages wire format natively.
struct AnthropicUpstream {
    std::string url;
    std::map<std::string, std::string> headers;
    utils::HttpSecurityPolicy policy = utils::HttpSecurityPolicy::ExternalHttpsOnly;
    std::string body;
};

// `claimed` separates "this model isn't relayed, fall through to conversion"
// from "it is relayed but the provider is unusable" — the latter must report
// its own error instead of failing further down as a misleading local-model
// load failure.
struct AnthropicUpstreamMatch {
    bool claimed = false;
    std::optional<AnthropicUpstream> upstream;
    int error_status = 0;
    std::string error_message;
};

static void set_anthropic_error_response(httplib::Response& res, int status,
                                         const std::string& message) {
    res.status = status;
    res.set_content(
        anthropic::build_anthropic_error(json{{"message", message}}, status).dump(),
        "application/json");
}

using anthropic::is_forwardable_request_header;
using anthropic::is_forwardable_response_header;

static AnthropicUpstreamMatch resolve_anthropic_upstream(ModelManager* model_manager,
                                                         const std::string& model,
                                                         const json& request_json,
                                                         const httplib::Request& req) {
    AnthropicUpstreamMatch match;
    if (model_manager == nullptr) return match;
    CloudProviderRegistry* registry = model_manager->cloud_registry();
    if (registry == nullptr) return match;

    ModelInfo info;
    try {
        if (!model_manager->model_exists(model)) return match;
        info = model_manager->get_model_info(model);
    } catch (const std::exception&) {
        return match;
    }
    if (info.recipe != "cloud" || info.cloud_provider.empty()) return match;
    if (registry->wire_format_for(info.cloud_provider) != "anthropic") return match;

    match.claimed = true;
    auto fail = [&match](int status, std::string message) {
        match.error_status = status;
        match.error_message = std::move(message);
        return match;
    };

    const std::string base_url = registry->base_url_for(info.cloud_provider);
    if (base_url.empty()) {
        return fail(500, "provider '" + info.cloud_provider +
                         "' has no base URL configured");
    }
    const std::string api_key = registry->resolve_key(info.cloud_provider);
    if (api_key.empty()) {
        return fail(401, "no API key for provider '" + info.cloud_provider + "'; set " +
                         CloudProviderRegistry::env_var_name(info.cloud_provider) +
                         " or POST /v1/cloud/auth");
    }
    const bool allow_insecure_http =
        registry->allow_insecure_http_for(info.cloud_provider);
    if (CloudProviderRegistry::is_http_base_url(base_url) && !allow_insecure_http) {
        return fail(400, "provider '" + info.cloud_provider +
                         "' uses an http:// base URL; re-install it with "
                         "--allow-insecure-http to send the API key in plaintext");
    }

    // CloudServer::load() rejects this for the OpenAI-shaped endpoints; without
    // the same check here a hand-authored registry entry would relay an empty
    // model id and surface an opaque provider 400 instead.
    if (info.checkpoint().empty()) {
        return fail(500, "cloud model '" + model + "' is missing the 'checkpoint' "
                         "field (provider's upstream model id)");
    }

    // The body passes through byte-for-byte apart from "model", which must name
    // the provider's own id rather than lemonade's "<provider>.<id>" public one.
    json forwarded = request_json;
    forwarded["model"] = info.checkpoint();

    const auto auth_header = registry->auth_header_for(info.cloud_provider);
    AnthropicUpstream upstream;
    upstream.url = backends::CloudServer::upstream_url(base_url, "/messages");
    // The Anthropic API reads `beta` from the query string, so it has to be
    // reattached to the upstream URL rather than folded into a header.
    if (req.has_param("beta") && req.get_param_value("beta") == "true") {
        upstream.url += "?beta=true";
    }
    upstream.headers = backends::CloudServer::upstream_headers(auth_header, api_key,
                                                               "anthropic");
    upstream.headers["Content-Type"] = "application/json";
    // The first client value replaces whatever default upstream_headers() set;
    // a repeat of the same name is joined, since a client may send
    // anthropic-beta as several headers rather than one comma-separated value.
    std::set<std::string> from_client;
    for (const auto& [name, value] : req.headers) {
        std::string lower = name;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        if (!is_forwardable_request_header(lower)) continue;
        if (from_client.insert(lower).second) {
            upstream.headers[lower] = value;
        } else {
            upstream.headers[lower] += "," + value;
        }
    }
    upstream.policy = backends::CloudServer::discovery_policy(base_url, allow_insecure_http);
    upstream.body = forwarded.dump();
    match.upstream = std::move(upstream);
    return match;
}

static void relay_response_headers(const std::map<std::string, std::string>& upstream,
                                   httplib::Response& res) {
    for (const auto& [name, value] : upstream) {
        if (is_forwardable_response_header(name)) {
            res.set_header(name, value);
        }
    }
}

static void forward_anthropic_upstream(AnthropicUpstream upstream,
                                       bool stream,
                                       const std::string& model,
                                       httplib::Response& res) {
    if (!stream) {
        try {
            auto response = utils::HttpClient::post(
                upstream.url, upstream.body, upstream.headers, 0, upstream.policy);
            res.status = response.status_code;
            relay_response_headers(response.headers, res);
            res.set_content(response.body, "application/json");
        } catch (const std::exception& e) {
            set_anthropic_error_response(
                res, 502, "cloud request for '" + model + "' failed: " + e.what());
        }
        return;
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    // Everything below runs inside the content provider, after the status line
    // is already on the wire. That costs the real status on a non-200 (relayed
    // as an SSE error frame instead) but keeps every blocking wait somewhere
    // sink.is_writable can observe the peer, so a client that disappears cannot
    // strand the upstream transfer.
    res.set_chunked_content_provider(
        "text/event-stream",
        [upstream = std::move(upstream), model](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) return false;
            // Both ends speak Anthropic SSE, so a 200 relays unparsed. A
            // non-200 body is not SSE, so it is diverted and re-emitted as an
            // error event rather than written into the event stream.
            static constexpr size_t max_error_body = 64 * 1024;
            int upstream_status = 200;
            std::string error_body;
            std::map<std::string, std::string> response_headers;
            auto emit_error = [&sink, &model](int status, const std::string& message) {
                write_sse_event(sink, "error", anthropic::build_anthropic_error(
                    json{{"message", "cloud request for '" + model + "' " + message}},
                    status));
            };

            try {
                auto result = utils::HttpClient::post_stream(
                    upstream.url, upstream.body,
                    [&](const char* data, size_t length) {
                        if (upstream_status != 200) {
                            if (error_body.size() < max_error_body) {
                                error_body.append(
                                    data,
                                    std::min(length, max_error_body - error_body.size()));
                            }
                            return true;
                        }
                        return sink.write(data, length);
                    },
                    upstream.headers, 0,
                    [&upstream_status](int status) { upstream_status = status; },
                    upstream.policy,
                    [&sink]() { return sink.is_writable && !sink.is_writable(); },
                    &response_headers);
                // on_status never fires when the body is empty, so fall back to
                // the code curl always records after the transfer.
                const int status =
                    upstream_status != 200 ? upstream_status : result.status_code;
                if (status != 200) {
                    emit_error(status, "failed with status " + std::to_string(status) +
                                       (error_body.empty() ? "" : ": " + error_body));
                } else if (result.curl_code != 0 && sink.is_writable && sink.is_writable()) {
                    emit_error(502, "stream ended early: " + result.curl_error);
                }
            } catch (const std::exception& e) {
                emit_error(502, std::string("failed: ") + e.what());
            }
            sink.done();
            return false;
        }
    );
}

}  // namespace

void OllamaApi::register_anthropic_routes(httplib::Server& server, const std::shared_ptr<OllamaApi>& self) {
    server.Post("/v1/messages", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_messages(req, res);
    });
}

json OllamaApi::convert_anthropic_to_openai_chat(const json& anthropic_request, std::vector<std::string>& warnings) {
    json openai_req;

    std::string model = normalize_model_name(anthropic_request.value("model", ""));
    openai_req["model"] = model;

    json messages = json::array();

    if (anthropic_request.contains("system")) {
        std::string system_text = join_text_blocks(anthropic_request["system"], warnings, "system");
        if (!system_text.empty()) {
            messages.push_back({{"role", "system"}, {"content", system_text}});
        }
    }

    if (anthropic_request.contains("messages") && anthropic_request["messages"].is_array()) {
        for (const auto& msg : anthropic_request["messages"]) {
            if (!msg.is_object()) {
                add_warning(warnings, "Ignored non-object item in 'messages'");
                continue;
            }

            std::string role = msg.value("role", "user");
            if (role != "user" && role != "assistant" && role != "system") {
                add_warning(warnings, "Unsupported role '" + role + "' mapped to 'user'");
                role = "user";
            }

            if (role == "system") {
                add_warning(warnings, "Ignored 'system' role in messages; use top-level system field");
                continue;
            }

            if (msg.contains("content") && msg["content"].is_string()) {
                messages.push_back({
                    {"role", role},
                    {"content", msg["content"]}
                });
                continue;
            }

            json content_parts = json::array();
            std::vector<std::string> text_parts;
            bool has_non_text = false;
            json assistant_tool_calls = json::array();
            std::vector<json> tool_result_messages;

            if (msg.contains("content") && msg["content"].is_array()) {
                for (const auto& block : msg["content"]) {
                    if (!block.is_object()) {
                        add_warning(warnings, "Ignored non-object message content block");
                        continue;
                    }

                    std::string type = block.value("type", "");
                    if (type == "text" && block.contains("text") && block["text"].is_string()) {
                        const std::string text = block["text"].get<std::string>();
                        content_parts.push_back({{"type", "text"}, {"text", text}});
                        text_parts.push_back(text);
                        continue;
                    }

                    if (type == "image" && block.contains("source") && block["source"].is_object()) {
                        const auto& source = block["source"];
                        std::string source_type = source.value("type", "");
                        std::string media_type = source.value("media_type", "");
                        std::string data = source.value("data", "");

                        if (source_type == "base64" && !media_type.empty() && !data.empty()) {
                            has_non_text = true;
                            content_parts.push_back({
                                {"type", "image_url"},
                                {"image_url", {{"url", "data:" + media_type + ";base64," + data}}}
                            });
                            continue;
                        }

                        add_warning(warnings, "Ignored image block with unsupported source format");
                        continue;
                    }

                    if (type == "tool_use") {
                        if (role != "assistant") {
                            add_warning(warnings, "Ignored tool_use block outside assistant role");
                            continue;
                        }

                        std::string tool_name = block.value("name", "");
                        if (tool_name.empty()) {
                            add_warning(warnings, "Ignored tool_use block missing name");
                            continue;
                        }

                        std::string tool_id = block.value("id", generate_anthropic_message_id());
                        json input_obj = json::object();
                        if (block.contains("input")) {
                            if (block["input"].is_object()) {
                                input_obj = block["input"];
                            } else {
                                add_warning(warnings, "tool_use.input was not an object; wrapped as _value");
                                input_obj = json{{"_value", block["input"]}};
                            }
                        }

                        assistant_tool_calls.push_back({
                            {"id", tool_id},
                            {"type", "function"},
                            {"function", {
                                {"name", tool_name},
                                {"arguments", input_obj.dump()}
                            }}
                        });
                        continue;
                    }

                    if (type == "tool_result") {
                        if (role != "user") {
                            add_warning(warnings, "Ignored tool_result block outside user role");
                            continue;
                        }

                        std::string tool_use_id = block.value("tool_use_id", "");
                        if (tool_use_id.empty()) {
                            add_warning(warnings, "Ignored tool_result block missing tool_use_id");
                            continue;
                        }

                        std::string tool_content = block.contains("content")
                            ? stringify_anthropic_tool_result_content(block["content"], warnings)
                            : std::string();

                        tool_result_messages.push_back({
                            {"role", "tool"},
                            {"tool_call_id", tool_use_id},
                            {"content", tool_content}
                        });
                        continue;
                    }

                    add_warning(warnings, "Ignored unsupported message content block type: " + type);
                }
            } else if (msg.contains("content")) {
                add_warning(warnings, "Ignored message content with unsupported type");
            }

            json openai_msg;
            openai_msg["role"] = role;

            if (!content_parts.empty()) {
                if (!has_non_text) {
                    openai_msg["content"] = join_strings(text_parts);
                } else {
                    openai_msg["content"] = content_parts;
                }
            } else {
                openai_msg["content"] = "";
            }

            if (!assistant_tool_calls.empty()) {
                openai_msg["tool_calls"] = assistant_tool_calls;
            }

            bool has_content = !content_parts.empty();
            bool has_tool_calls = !assistant_tool_calls.empty();

            bool is_tool_result_only = (role == "user" && !has_content && !has_tool_calls && !tool_result_messages.empty());
            if (!is_tool_result_only && (role == "assistant" || has_content || has_tool_calls)) {
                messages.push_back(openai_msg);
            }

            for (const auto& tool_msg : tool_result_messages) {
                messages.push_back(tool_msg);
            }
        }
    }

    openai_req["messages"] = messages;

    if (anthropic_request.contains("max_tokens")) {
        openai_req["max_completion_tokens"] = anthropic_request["max_tokens"];
    }
    if (anthropic_request.contains("temperature")) {
        openai_req["temperature"] = anthropic_request["temperature"];
    }
    if (anthropic_request.contains("top_p")) {
        openai_req["top_p"] = anthropic_request["top_p"];
    }
    if (anthropic_request.contains("top_k")) {
        openai_req["top_k"] = anthropic_request["top_k"];
    }

    if (anthropic_request.contains("stop_sequences")) {
        openai_req["stop"] = anthropic_request["stop_sequences"];
    }

    if (anthropic_request.contains("tools") && anthropic_request["tools"].is_array()) {
        json openai_tools = json::array();
        for (const auto& tool : anthropic_request["tools"]) {
            if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) {
                add_warning(warnings, "Ignored invalid tool definition in 'tools'");
                continue;
            }

            json parameters = json::object();
            if (tool.contains("input_schema") && tool["input_schema"].is_object()) {
                parameters = tool["input_schema"];
            }

            openai_tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool["name"]},
                    {"description", tool.value("description", "")},
                    {"parameters", parameters}
                }}
            });
        }

        if (!openai_tools.empty()) {
            openai_req["tools"] = openai_tools;
        }
    }

    if (anthropic_request.contains("tool_choice") && anthropic_request["tool_choice"].is_object()) {
        const auto& tc = anthropic_request["tool_choice"];
        std::string type = tc.value("type", "auto");
        if (type == "auto") {
            openai_req["tool_choice"] = "auto";
        } else if (type == "any") {
            openai_req["tool_choice"] = "required";
        } else if (type == "none") {
            openai_req["tool_choice"] = "none";
        } else if (type == "tool") {
            std::string name = tc.value("name", "");
            if (name.empty()) {
                add_warning(warnings, "Ignored tool_choice.type=tool without name");
            } else {
                openai_req["tool_choice"] = {
                    {"type", "function"},
                    {"function", {{"name", name}}}
                };
            }
        } else {
            add_warning(warnings, "Ignored unsupported tool_choice.type: " + type);
        }
    }

    if (anthropic_request.contains("output_config") && anthropic_request["output_config"].is_object()) {
        const auto& output_config = anthropic_request["output_config"];
        if (output_config.contains("format") && output_config["format"].is_object()) {
            const auto& format = output_config["format"];
            std::string type = format.value("type", "");

            if (type == "json_schema" && format.contains("schema") && format["schema"].is_object()) {
                openai_req["response_format"] = {
                    {"type", "json_schema"},
                    {"json_schema", {
                        {"name", "response"},
                        {"schema", format["schema"]}
                    }}
                };
            } else if (type == "json_object") {
                openai_req["response_format"] = { {"type", "json_object"} };
            } else {
                add_warning(warnings, "Ignored unsupported output_config.format type: " + type);
            }
        }
    }

    if (anthropic_request.contains("thinking") && anthropic_request["thinking"].is_object()) {
        std::string thinking_type = anthropic_request["thinking"].value("type", "");
        if (thinking_type == "enabled") {
            openai_req["enable_thinking"] = true;
        } else if (thinking_type == "disabled") {
            openai_req["enable_thinking"] = false;
        } else {
            add_warning(warnings, "Ignored unsupported thinking.type: " + thinking_type);
        }
    }

    if (anthropic_request.contains("metadata")) {
        add_warning(warnings, "Ignored 'metadata' field");
    }
    if (anthropic_request.contains("context_management")) {
        add_warning(warnings, "Ignored 'context_management' field");
    }

    openai_req["stream"] = anthropic_request.value("stream", false);

    return openai_req;
}

json OllamaApi::convert_openai_chat_to_anthropic(const json& openai_response,
                                                 const std::string& model,
                                                 const std::vector<std::string>& warnings) {
    std::vector<std::string> mutable_warnings = warnings;
    std::string response_text;
    json content_blocks = json::array();
    std::string stop_reason = "end_turn";
    std::string response_id = openai_response.value("id", generate_anthropic_message_id());

    if (openai_response.contains("choices") && openai_response["choices"].is_array() &&
        !openai_response["choices"].empty()) {
        const auto& choice = openai_response["choices"][0];
        stop_reason = map_finish_reason_to_anthropic_stop_reason(choice);

        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            if (message.contains("content") && message["content"].is_string()) {
                response_text = message["content"].get<std::string>();
            } else if (message.contains("content") && message["content"].is_array()) {
                std::vector<std::string> text_blocks;
                for (const auto& block : message["content"]) {
                    if (block.is_object() && block.value("type", "") == "text" &&
                        block.contains("text") && block["text"].is_string()) {
                        text_blocks.push_back(block["text"].get<std::string>());
                    }
                }
                response_text = join_strings(text_blocks);
            }

            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& tool_call : message["tool_calls"]) {
                    if (!tool_call.is_object()) {
                        continue;
                    }

                    std::string tool_id = tool_call.value("id", generate_anthropic_message_id());
                    std::string tool_name;
                    if (tool_call.contains("function") && tool_call["function"].is_object()) {
                        tool_name = tool_call["function"].value("name", "");
                    }
                    if (tool_name.empty()) {
                        add_warning(mutable_warnings, "Encountered tool_call without function name");
                        continue;
                    }

                    content_blocks.push_back({
                        {"type", "tool_use"},
                        {"id", tool_id},
                        {"name", tool_name},
                        {"input", parse_openai_tool_arguments(tool_call, mutable_warnings)}
                    });
                }
            }
        }
    }

    if (!response_text.empty() || content_blocks.empty()) {
        json merged_blocks = json::array();
        merged_blocks.push_back({
            {"type", "text"},
            {"text", response_text}
        });
        for (const auto& block : content_blocks) {
            merged_blocks.push_back(block);
        }
        content_blocks = merged_blocks;
    }

    if (stop_reason == "end_turn") {
        for (const auto& block : content_blocks) {
            if (block.is_object() && block.value("type", "") == "tool_use") {
                stop_reason = "tool_use";
                break;
            }
        }
    }

    int input_tokens = 0;
    int output_tokens = 0;
    if (openai_response.contains("usage") && openai_response["usage"].is_object()) {
        const auto& usage = openai_response["usage"];
        input_tokens = usage.value("prompt_tokens", 0);
        output_tokens = usage.value("completion_tokens", 0);
    }

    json anthropic_res = {
        {"id", response_id},
        {"type", "message"},
        {"role", "assistant"},
        {"model", model},
        {"content", content_blocks},
        {"stop_reason", stop_reason},
        {"stop_sequence", nullptr},
        {"usage", {
            {"input_tokens", input_tokens},
            {"output_tokens", output_tokens}
        }}
    };

    if (!mutable_warnings.empty()) {
        anthropic_res["warnings"] = mutable_warnings;
    }

    return anthropic_res;
}

void OllamaApi::stream_openai_sse_to_anthropic_sse(const std::string& openai_body,
                                                   httplib::DataSink& client_sink,
                                                   const std::string& model,
                                                   const std::vector<std::string>& warnings,
                                                   StreamFn call_router) {
    httplib::DataSink adapter_sink;
    std::string sse_buffer;

    bool sent_message_start = false;
    bool sent_text_content_start = false;
    bool sent_text_content_stop = false;
    bool sent_error = false;
    std::vector<bool> started_tool_blocks;
    std::vector<bool> stopped_tool_blocks;
    std::vector<std::string> tool_ids;
    std::vector<std::string> tool_names;
    std::string stop_reason = "end_turn";
    int input_tokens = 0;
    int output_tokens = 0;
    std::string message_id = generate_anthropic_message_id();

    adapter_sink.is_writable = client_sink.is_writable;

    adapter_sink.write = [&client_sink,
                          &sse_buffer,
                          &sent_message_start,
                          &sent_text_content_start,
                          &sent_text_content_stop,
                          &sent_error,
                          &started_tool_blocks,
                          &stopped_tool_blocks,
                          &tool_ids,
                          &tool_names,
                          &stop_reason,
                          &input_tokens,
                          &output_tokens,
                          &message_id,
                          &model](const char* data, size_t len) -> bool {
        sse_buffer.append(data, len);

        size_t pos;
        while ((pos = sse_buffer.find('\n')) != std::string::npos) {
            std::string line = sse_buffer.substr(0, pos);
            sse_buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty() || line.find("data: ") != 0) {
                continue;
            }

            std::string json_str = line.substr(6);
            if (json_str == "[DONE]") {
                continue;
            }

            try {
                auto openai_chunk = json::parse(json_str);

                if (openai_chunk.contains("error")) {
                    const auto& error = openai_chunk["error"];
                    std::cerr << "[OllamaApi] Backend error in Anthropic stream: "
                              << error.dump() << std::endl;
                    sent_error = true;
                    write_sse_event(client_sink, "error",
                                    anthropic::build_anthropic_error(
                                        error, anthropic::backend_error_http_status(error)));
                    return false;
                }

                if (!sent_message_start) {
                    if (openai_chunk.contains("id") && openai_chunk["id"].is_string()) {
                        message_id = openai_chunk["id"].get<std::string>();
                    }

                    json message_start = {
                        {"type", "message_start"},
                        {"message", {
                            {"id", message_id},
                            {"type", "message"},
                            {"role", "assistant"},
                            {"model", model},
                            {"content", json::array()},
                            {"stop_reason", nullptr},
                            {"stop_sequence", nullptr},
                            {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
                        }}
                    };
                    if (!write_sse_event(client_sink, "message_start", message_start)) {
                        return false;
                    }
                    sent_message_start = true;
                }

                if (openai_chunk.contains("usage") && openai_chunk["usage"].is_object()) {
                    const auto& usage = openai_chunk["usage"];
                    input_tokens = usage.value("prompt_tokens", input_tokens);
                    output_tokens = usage.value("completion_tokens", output_tokens);
                }

                if (openai_chunk.contains("choices") && openai_chunk["choices"].is_array() &&
                    !openai_chunk["choices"].empty()) {
                    const auto& choice = openai_chunk["choices"][0];

                    if (choice.contains("delta") && choice["delta"].is_object()) {
                        const auto& delta = choice["delta"];
                        if (delta.contains("content") && delta["content"].is_string()) {
                            std::string delta_text = delta["content"].get<std::string>();
                            if (!delta_text.empty()) {
                                if (!sent_text_content_start) {
                                    json content_start = {
                                        {"type", "content_block_start"},
                                        {"index", 0},
                                        {"content_block", {{"type", "text"}, {"text", ""}}}
                                    };
                                    if (!write_sse_event(client_sink, "content_block_start", content_start)) {
                                        return false;
                                    }
                                    sent_text_content_start = true;
                                }

                                json content_delta = {
                                    {"type", "content_block_delta"},
                                    {"index", 0},
                                    {"delta", {{"type", "text_delta"}, {"text", delta_text}}}
                                };
                                if (!write_sse_event(client_sink, "content_block_delta", content_delta)) {
                                    return false;
                                }
                            }
                        }

                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (const auto& tool_delta : delta["tool_calls"]) {
                                if (!tool_delta.is_object()) {
                                    continue;
                                }

                                int openai_tool_index = tool_delta.value("index", 0);
                                if (openai_tool_index < 0) {
                                    continue;
                                }

                                size_t idx = static_cast<size_t>(openai_tool_index);
                                if (started_tool_blocks.size() <= idx) {
                                    started_tool_blocks.resize(idx + 1, false);
                                    stopped_tool_blocks.resize(idx + 1, false);
                                    tool_ids.resize(idx + 1);
                                    tool_names.resize(idx + 1);
                                }

                                if (tool_delta.contains("id") && tool_delta["id"].is_string()) {
                                    tool_ids[idx] = tool_delta["id"].get<std::string>();
                                }

                                if (tool_delta.contains("function") && tool_delta["function"].is_object()) {
                                    const auto& fn = tool_delta["function"];
                                    if (fn.contains("name") && fn["name"].is_string()) {
                                        tool_names[idx] = fn["name"].get<std::string>();
                                    }
                                }

                                if (!started_tool_blocks[idx]) {
                                    if (tool_ids[idx].empty()) {
                                        tool_ids[idx] = generate_anthropic_message_id();
                                    }
                                    if (tool_names[idx].empty()) {
                                        tool_names[idx] = "unknown_tool";
                                    }

                                    json tool_block_start = {
                                        {"type", "content_block_start"},
                                        {"index", static_cast<int>(idx) + 1},
                                        {"content_block", {
                                            {"type", "tool_use"},
                                            {"id", tool_ids[idx]},
                                            {"name", tool_names[idx]},
                                            {"input", json::object()}
                                        }}
                                    };
                                    if (!write_sse_event(client_sink, "content_block_start", tool_block_start)) {
                                        return false;
                                    }
                                    started_tool_blocks[idx] = true;
                                }

                                if (tool_delta.contains("function") && tool_delta["function"].is_object()) {
                                    const auto& fn = tool_delta["function"];
                                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                        std::string args_delta = fn["arguments"].get<std::string>();
                                        if (!args_delta.empty()) {
                                            json tool_input_delta = {
                                                {"type", "content_block_delta"},
                                                {"index", static_cast<int>(idx) + 1},
                                                {"delta", {
                                                    {"type", "input_json_delta"},
                                                    {"partial_json", args_delta}
                                                }}
                                            };
                                            if (!write_sse_event(client_sink, "content_block_delta", tool_input_delta)) {
                                                return false;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        stop_reason = map_finish_reason_to_anthropic_stop_reason(choice);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[OllamaApi] Failed to parse Anthropic stream chunk: " << e.what() << std::endl;
            }
        }
        return true;
    };

    adapter_sink.done = [&client_sink,
                         &sent_message_start,
                         &sent_text_content_start,
                         &sent_text_content_stop,
                         &sent_error,
                         &started_tool_blocks,
                         &stopped_tool_blocks,
                         &stop_reason,
                         &input_tokens,
                         &output_tokens,
                         &warnings]() {
        // The error event terminates the stream. Emitting the closing frames
        // here would let a client read the failure as a turn that produced no
        // content, which is the state this path exists to avoid.
        if (sent_error) {
            client_sink.done();
            return;
        }

        if (!sent_message_start) {
            json message_start = {
                {"type", "message_start"},
                {"message", {
                    {"id", generate_anthropic_message_id()},
                    {"type", "message"},
                    {"role", "assistant"},
                    {"content", json::array()},
                    {"stop_reason", nullptr},
                    {"stop_sequence", nullptr},
                    {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
                }}
            };
            if (!write_sse_event(client_sink, "message_start", message_start)) {
                client_sink.done();
                return;
            }
            sent_message_start = true;
        }

        if (!sent_text_content_start && started_tool_blocks.empty()) {
            json content_start = {
                {"type", "content_block_start"},
                {"index", 0},
                {"content_block", {{"type", "text"}, {"text", ""}}}
            };
            if (!write_sse_event(client_sink, "content_block_start", content_start)) {
                client_sink.done();
                return;
            }
            sent_text_content_start = true;
        }

        if (sent_text_content_start && !sent_text_content_stop) {
            json content_stop = {
                {"type", "content_block_stop"},
                {"index", 0}
            };
            if (!write_sse_event(client_sink, "content_block_stop", content_stop)) {
                client_sink.done();
                return;
            }
            sent_text_content_stop = true;
        }

        for (size_t idx = 0; idx < started_tool_blocks.size(); ++idx) {
            if (started_tool_blocks[idx] && !stopped_tool_blocks[idx]) {
                json tool_stop = {
                    {"type", "content_block_stop"},
                    {"index", static_cast<int>(idx) + 1}
                };
                if (!write_sse_event(client_sink, "content_block_stop", tool_stop)) {
                    client_sink.done();
                    return;
                }
                stopped_tool_blocks[idx] = true;
            }
        }

        if (stop_reason == "end_turn") {
            for (bool started : started_tool_blocks) {
                if (started) {
                    stop_reason = "tool_use";
                    break;
                }
            }
        }

        json message_delta = {
            {"type", "message_delta"},
            {"delta", {
                {"stop_reason", stop_reason},
                {"stop_sequence", nullptr}
            }},
            {"usage", {
                {"input_tokens", input_tokens},
                {"output_tokens", output_tokens}
            }}
        };
        if (!warnings.empty()) {
            message_delta["warnings"] = warnings;
        }
        if (!write_sse_event(client_sink, "message_delta", message_delta)) {
            client_sink.done();
            return;
        }

        json message_stop = {{"type", "message_stop"}};
        write_sse_event(client_sink, "message_stop", message_stop);
        client_sink.done();
    };

    call_router(openai_body, adapter_sink);
}

void OllamaApi::handle_anthropic_messages(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        std::vector<std::string> warnings;

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"type":"error","error":{"type":"invalid_request_error","message":"model is required"}})", "application/json");
            return;
        }

        if (req.has_param("beta")) {
            std::string beta_value = req.get_param_value("beta");
            if (beta_value != "true") {
                add_warning(warnings, "Ignored unsupported beta query value: " + beta_value);
            }
        }

        auto emit_warning_header = [&res, &warnings]() {
            if (warnings.empty()) return;
            std::ostringstream warning_header;
            for (size_t i = 0; i < warnings.size(); ++i) {
                if (i > 0) warning_header << " | ";
                warning_header << warnings[i];
            }
            res.set_header("X-Lemonade-Warning", warning_header.str());
            std::cerr << "[OllamaApi] Anthropic compatibility warnings: "
                      << warning_header.str() << std::endl;
        };

        // Relaying verbatim keeps thinking blocks, tool use, and cache control
        // intact. No router slot is taken — there is no local resource to hold.
        auto match = resolve_anthropic_upstream(model_manager_, model, request_json, req);
        if (match.claimed) {
            emit_warning_header();
            if (!match.upstream) {
                set_anthropic_error_response(res, match.error_status,
                                             match.error_message);
                return;
            }
            forward_anthropic_upstream(std::move(*match.upstream),
                                       request_json.value("stream", false), model, res);
            return;
        }

        auto openai_req = convert_anthropic_to_openai_chat(request_json, warnings);

        try {
            auto_load_model(model, extract_auto_load_options(request_json));
        } catch (const RouterResidencyConflictException& e) {
            set_anthropic_residency_conflict_response(e, res);
            return;
        } catch (const std::exception&) {
            res.status = 404;
            json error = {
                {"type", "error"},
                {"error", {
                    {"type", "not_found_error"},
                    {"message", "model '" + model + "' not found, try pulling it first"}
                }}
            };
            res.set_content(error.dump(), "application/json");
            return;
        }

        bool stream = openai_req.value("stream", false);
        emit_warning_header();

        if (stream) {
            openai_req["stream"] = true;
            std::string openai_body = openai_req.dump();

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [this, openai_body, model, warnings](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;

                    stream_openai_sse_to_anthropic_sse(openai_body, sink, model, warnings,
                        [this](const std::string& body, httplib::DataSink& s) {
                            router_->chat_completion_stream(body, s);
                        }
                    );

                    return false;
                }
            );
            return;
        }

        openai_req["stream"] = false;
        auto openai_response = router_->chat_completion(openai_req);
        if (set_anthropic_backend_error_response(openai_response, res)) {
            return;
        }

        auto anthropic_response = convert_openai_chat_to_anthropic(openai_response, model, warnings);
        res.set_content(anthropic_response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /v1/messages: " << e.what() << std::endl;
        res.status = 500;
        json error = {
            {"type", "error"},
            {"error", {
                {"type", "api_error"},
                {"message", std::string(e.what())}
            }}
        };
        res.set_content(error.dump(), "application/json");
    }
}

}  // namespace lemon
