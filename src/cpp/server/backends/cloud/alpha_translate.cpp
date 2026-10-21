// Pure translation between the OpenAI chat shape lemond serves and the
// proprietary agent envelope CommandCode's /alpha/generate speaks. Kept
// network-free so the whole mapping stays unit-testable without a live
// vendor endpoint.

#include "lemon/backends/cloud/alpha_translate.h"

#include <atomic>
#include <chrono>
#include <ctime>

namespace lemon {
namespace backends {
namespace cloud {
namespace alpha {

namespace {

bool is_role(const json& msg, const char* role) {
    return msg.is_object() && msg.contains("role") && msg["role"].is_string() &&
           msg["role"].get<std::string>() == role;
}

std::string message_text(const json& msg) {
    if (!msg.is_object() || !msg.contains("content")) {
        return "";
    }
    const auto& content = msg["content"];
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (content.is_array()) {
        std::string out;
        for (const auto& part : content) {
            if (part.is_object() && part.value("type", "") == "text" &&
                part.contains("text")) {
                const auto& text = part["text"];
                if (text.is_string()) {
                    out += text.get<std::string>();
                }
            } else if (part.is_string()) {
                out += part.get<std::string>();
            }
        }
        return out;
    }
    return "";
}

void copy_number(const json& from, const char* key, json& to) {
    auto it = from.find(key);
    if (it != from.end() && it->is_number()) {
        to[key] = *it;
    }
}

std::string finish_reason(const StreamState& state) {
    if (state.finish_reason == "stop" || state.finish_reason == "length" ||
        state.finish_reason == "tool_calls" ||
        state.finish_reason == "content_filter") {
        return state.finish_reason;
    }
    return "stop";
}

void set_usage(const StreamState& state, json& out) {
    json usage = json::object();
    usage["prompt_tokens"] = state.input_tokens >= 0 ? state.input_tokens : 0;
    usage["completion_tokens"] = state.output_tokens >= 0 ? state.output_tokens : 0;
    usage["total_tokens"] = usage["prompt_tokens"].get<int64_t>() +
                            usage["completion_tokens"].get<int64_t>();
    out["usage"] = std::move(usage);
}

}  // namespace

json build_envelope(const std::string& upstream_model, const json& openai_request) {
    json params = json::object();
    params["model"] = upstream_model;
    params["stream"] = true;

    std::string system;
    json messages = json::array();
    if (openai_request.contains("messages") && openai_request["messages"].is_array()) {
        for (const auto& msg : openai_request["messages"]) {
            if (is_role(msg, "system") || is_role(msg, "developer")) {
                const std::string text = message_text(msg);
                if (!text.empty()) {
                    if (!system.empty()) {
                        system += "\n";
                    }
                    system += text;
                }
                continue;
            }
            if (is_role(msg, "user") || is_role(msg, "assistant") ||
                is_role(msg, "tool")) {
                json out = json::object();
                out["role"] = msg["role"];
                out["content"] = message_text(msg);
                messages.push_back(std::move(out));
            }
        }
    } else if (openai_request.contains("prompt")) {
        const auto& prompt = openai_request["prompt"];
        std::string text;
        if (prompt.is_string()) {
            text = prompt.get<std::string>();
        } else if (prompt.is_array() && !prompt.empty() && prompt.back().is_string()) {
            text = prompt.back().get<std::string>();
        }
        messages.push_back({{"role", "user"}, {"content", text}});
    }
    if (!system.empty()) {
        params["system"] = system;
    }
    params["messages"] = std::move(messages);

    copy_number(openai_request, "temperature", params);
    copy_number(openai_request, "top_p", params);
    if (openai_request.contains("max_completion_tokens") &&
        openai_request["max_completion_tokens"].is_number()) {
        params["max_tokens"] = openai_request["max_completion_tokens"];
    } else {
        copy_number(openai_request, "max_tokens", params);
    }

    json config = {
        {"workingDir", "/workspace"},
        {"date", ""},
        {"environment", "external"},
        {"structure", json::array()},
        {"isGitRepo", false},
        {"currentBranch", ""},
        {"mainBranch", ""},
        {"gitStatus", ""},
        {"recentCommits", json::array()},
    };
    return json{{"config", std::move(config)},
                {"memory", ""},
                {"params", std::move(params)}};
}

bool is_model_not_found(int http_status, const json& response_body) {
    if (http_status == 404) {
        return true;
    }
    if (!response_body.is_object() || !response_body.contains("error")) {
        return false;
    }
    // Lemonade wraps the provider body under error.details, preserving the
    // upstream status there. A wrapped 404 triggers even when the outer call
    // status was lost in transit.
    if (http_status == 0) {
        const auto& error = response_body["error"];
        if (error.is_object() && error.contains("details") &&
            error["details"].is_object()) {
            auto it = error["details"].find("status_code");
            if (it != error["details"].end() && it->is_number_integer() &&
                it->get<int64_t>() == 404) {
                return true;
            }
        }
    }
    if (http_status != 400 && http_status != 0) {
        return false;
    }
    const auto& error = response_body["error"];
    // Match against the message plus machine-readable fields: vendors put
    // the signal in either place ("not supported on this endpoint" prose
    // with an "unsupported_model" code, or a bare "not found" message).
    std::string haystack;
    if (error.is_object()) {
        for (const char* key : {"message", "code", "param"}) {
            auto it = error.find(key);
            if (it != error.end() && it->is_string()) {
                haystack += it->get<std::string>();
                haystack += " ";
            }
        }
        // Lemonade wraps the provider body under error.details.response, and
        // vendors double-wrap theirs (JSON-encoded JSON in "message"). A raw
        // dump preserves the signal text in both cases.
        auto details = error.find("details");
        if (details != error.end()) {
            haystack += details->dump();
            haystack += " ";
        }
    }
    if (haystack.empty()) {
        haystack = response_body.dump();
    }
    for (const char* needle : {"not found", "not_found", "not supported",
                               "unsupported_model", "unsupported model",
                               "unknown model", "does not exist", "no such model"}) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void feed_event(const json& event, StreamState& state) {
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
        return;
    }
    const std::string type = event["type"].get<std::string>();
    if (type == "reasoning-delta" && event.contains("text") &&
        event["text"].is_string()) {
        state.reasoning += event["text"].get<std::string>();
    } else if (type == "text-delta" && event.contains("text") &&
               event["text"].is_string()) {
        state.text += event["text"].get<std::string>();
    } else if (type == "finish-step" || type == "finish") {
        if (event.contains("finishReason") && event["finishReason"].is_string()) {
            state.finish_reason = event["finishReason"].get<std::string>();
        } else if (event.contains("finish_reason") &&
                   event["finish_reason"].is_string()) {
            state.finish_reason = event["finish_reason"].get<std::string>();
        }
        const json* usage = nullptr;
        if (event.contains("usage") && event["usage"].is_object()) {
            usage = &event["usage"];
        } else if (event.contains("totalUsage") && event["totalUsage"].is_object()) {
            usage = &event["totalUsage"];
        }
        if (usage != nullptr) {
            auto pick = [&](std::initializer_list<const char*> keys) -> int64_t {
                for (const char* key : keys) {
                    auto it = usage->find(key);
                    if (it != usage->end() && it->is_number_integer()) {
                        const int64_t value = it->get<int64_t>();
                        if (value >= 0) {
                            return value;
                        }
                    }
                }
                return -1;
            };
            const int64_t in = pick({"inputTokens", "input_tokens", "prompt_tokens"});
            const int64_t out =
                pick({"outputTokens", "output_tokens", "completion_tokens"});
            const int64_t total = pick({"totalTokens", "total_tokens"});
            if (in >= 0) {
                state.input_tokens = in;
            }
            if (out >= 0) {
                state.output_tokens = out;
            }
            if (total >= 0 && state.input_tokens < 0 && state.output_tokens < 0) {
                state.input_tokens = 0;
                state.output_tokens = total;
            }
        }
        if (type == "finish") {
            state.done = true;
        }
    } else if (type == "error") {
        state.error = true;
        if (event.contains("message") && event["message"].is_string()) {
            state.error_message = event["message"].get<std::string>();
        } else if (event.contains("error")) {
            state.error_message = event["error"].dump();
        } else {
            state.error_message = event.dump();
        }
    }
}

std::string new_completion_id() {
    static std::atomic<uint64_t> counter{0};
    return "chatcmpl-alpha-" + std::to_string(static_cast<long>(std::time(nullptr))) +
           "-" + std::to_string(counter.fetch_add(1));
}

json to_chat_completion(const StreamState& state, const std::string& model,
                        const std::string& completion_id, int64_t created) {
    json message = {{"role", "assistant"}, {"content", state.text}};
    if (!state.reasoning.empty()) {
        message["reasoning_content"] = state.reasoning;
    }
    json out = {
        {"id", completion_id},
        {"object", "chat.completion"},
        {"created", created},
        {"model", model},
        {"choices",
         {{{"index", 0}, {"message", std::move(message)}, {"finish_reason", finish_reason(state)}}}},
    };
    set_usage(state, out);
    return out;
}

json to_text_completion(const StreamState& state, const std::string& model,
                        const std::string& completion_id, int64_t created) {
    json out = {
        {"id", completion_id},
        {"object", "text_completion"},
        {"created", created},
        {"model", model},
        {"choices",
         {{{"index", 0}, {"text", state.text}, {"finish_reason", finish_reason(state)}}}},
    };
    set_usage(state, out);
    return out;
}

std::string sse_role_chunk(const std::string& completion_id, int64_t created,
                           const std::string& model, bool legacy) {
    json chunk = {
        {"id", completion_id},
        {"object", legacy ? "text_completion" : "chat.completion.chunk"},
        {"created", created},
        {"model", model},
    };
    if (legacy) {
        chunk["choices"] = json::array({{{"index", 0}, {"text", ""}}});
    } else {
        chunk["choices"] =
            json::array({{{"index", 0}, {"delta", {{"role", "assistant"}}}}});
    }
    return "data: " + chunk.dump() + "\n\n";
}

std::string sse_content_chunk(const std::string& completion_id, int64_t created,
                              const std::string& model, const std::string& text,
                              bool legacy) {
    json chunk = {
        {"id", completion_id},
        {"object", legacy ? "text_completion" : "chat.completion.chunk"},
        {"created", created},
        {"model", model},
    };
    if (legacy) {
        chunk["choices"] = {{{"index", 0}, {"text", text}}};
    } else {
        chunk["choices"] = {{{"index", 0}, {"delta", {{"content", text}}}}};
    }
    return "data: " + chunk.dump() + "\n\n";
}

std::string sse_reasoning_chunk(const std::string& completion_id, int64_t created,
                                const std::string& model, const std::string& text) {
    const json chunk = {
        {"id", completion_id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices",
         {{{"index", 0}, {"delta", {{"reasoning_content", text}}}}}},
    };
    return "data: " + chunk.dump() + "\n\n";
}

std::string sse_final_chunk(const StreamState& state, const std::string& completion_id,
                            int64_t created, const std::string& model, bool legacy,
                            bool include_usage) {
    const std::string reason = finish_reason(state);
    json chunk = {
        {"id", completion_id},
        {"object", legacy ? "text_completion" : "chat.completion.chunk"},
        {"created", created},
        {"model", model},
    };
    if (legacy) {
        chunk["choices"] = {{{"index", 0}, {"text", ""}, {"finish_reason", reason}}};
    } else {
        chunk["choices"] = {{{"index", 0}, {"delta", json::object()}, {"finish_reason", reason}}};
    }
    if (include_usage) {
        set_usage(state, chunk);
    }
    return "data: " + chunk.dump() + "\n\n";
}

}  // namespace alpha
}  // namespace cloud
}  // namespace backends
}  // namespace lemon
