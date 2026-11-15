#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace lemon::hybrid_history {

using json = nlohmann::json;

// OpenAI-compatible clients may repack text into content parts between requests.
// A single logical text payload is equivalent regardless of that wire encoding.
inline json canonical_content(const json& content) {
    if (!content.is_array()) return content;
    std::string text;
    for (const auto& part : content) {
        if (!part.is_object() || part.value("type", "") != "text" ||
            !part.contains("text") || !part["text"].is_string()) {
            return content;  // Images and other modalities must match exactly.
        }
        text += part["text"].get<std::string>();
    }
    return text;
}

// OpenAI function.arguments is a JSON-encoded string. Parse it only
// for prefix validation; never rewrite the model request or client response.
// Invalid JSON cannot collide with a valid JSON string or scalar value.
inline json canonical_tool_arguments(const json& arguments) {
    if (!arguments.is_string()) {
        throw std::invalid_argument("Hybrid tool-call arguments must be JSON strings");
    }
    const json value = json::parse(arguments.get<std::string>(), nullptr, false);
    if (value.is_discarded()) {
        throw std::invalid_argument("Hybrid tool-call arguments must contain valid JSON");
    }
    return value;
}

inline json normalized_message(const json& message) {
    if (!message.is_object()) return message;
    json result = {{"role", message.value("role", "")}};
    const bool has_tools = message.contains("tool_calls") && message["tool_calls"].is_array() &&
                           !message["tool_calls"].empty();
    if (message.contains("content") && !message["content"].is_null()) {
        result["content"] = message["content"];
    } else {
        result["content"] = has_tools ? json("") : json(nullptr);
    }
    if (message.contains("tool_call_id")) {
        result["tool_call_id"] = message["tool_call_id"];
    }
    if (has_tools) {
        result["tool_calls"] = json::array();
        for (const auto& call : message["tool_calls"]) {
            if (!call.is_object() || !call.contains("function") ||
                !call["function"].is_object() ||
                !call["function"].contains("arguments") ||
                !call["function"]["arguments"].is_string()) {
                throw std::invalid_argument("Hybrid tool-call arguments must be JSON strings");
            }
            const json& function = call["function"];
            result["tool_calls"].push_back({
                {"id", call.value("id", "")},
                {"type", call.value("type", "function")},
                {"function", {{"name", function.value("name", "")},
                              {"arguments", function["arguments"]}}}
            });
        }
    }
    return result;
}

// Comparison-only normalization. Never use this for responses or model input:
// OpenAI function.arguments must remain a JSON *string* on the wire.
inline json canonical_history_message(const json& message) {
    json result = normalized_message(message);
    if (!result.is_object()) return result;
    if (result.contains("content")) {
        result["content"] = canonical_content(result["content"]);
    }
    if (result.contains("tool_calls") && result["tool_calls"].is_array()) {
        for (auto& call : result["tool_calls"]) {
            json& function = call["function"];
            function["arguments"] = canonical_tool_arguments(function["arguments"]);
        }
    }
    // An assistant with no content is represented as either null or an empty
    // string by different OpenAI clients.
    if (result.value("role", "") == "assistant" &&
        result["content"].is_null()) {
        result["content"] = "";
    }
    return result;
}

inline bool history_messages_equivalent(const json& expected, const json& received) {
    if (expected == received) return true;
    // The server owns the authoritative tool call and its preceding text. Some
    // SDKs omit that accompanying text when echoing assistant tool calls. This
    // narrow exception accepts omission, never new or rewritten instructions.
    if (expected.is_object() && received.is_object() &&
        expected.value("role", "") == "assistant" &&
        received.value("role", "") == "assistant" &&
        expected.contains("tool_calls") && received.contains("tool_calls") &&
        expected["tool_calls"] == received["tool_calls"] &&
        received.value("content", json()) == json("") &&
        expected.value("content", json()).is_string()) {
        return true;
    }
    return false;
}

// A new, independent Chat Completions conversation has only leading instructions
// followed by its initial user message(s). Never classify a request containing
// assistant/tool history as a new conversation: it must pass append-only checks.
inline bool is_fresh_conversation_request(const json& messages) {
    if (!messages.is_array() || messages.empty()) return false;
    bool saw_user = false;
    for (const auto& message : messages) {
        if (!message.is_object() || !message.contains("role") ||
            !message["role"].is_string()) return false;
        const std::string role = message["role"].get<std::string>();
        if (role == "system" || role == "developer") {
            if (saw_user) return false;
        } else if (role == "user") {
            saw_user = true;
        } else {
            return false;
        }
    }
    return saw_user && messages.back().value("role", "") == "user";
}

// Chat Completions does not supply a stable conversation ID. A distinct *initial*
// transcript is therefore a new session; erase both KV slots before using it.
// An identical prefix is ambiguous (retry/rewind versus another fresh chat), so
// it continues through strict ingest() rather than silently resetting anything.
inline bool is_distinct_fresh_conversation(const json& previous,
                                           const json& incoming) {
    if (!previous.is_array() || !is_fresh_conversation_request(incoming)) return false;
    const size_t overlap = std::min(previous.size(), incoming.size());
    for (size_t i = 0; i < overlap; ++i) {
        if (!history_messages_equivalent(canonical_history_message(previous[i]),
                                         canonical_history_message(incoming[i]))) {
            return true;
        }
    }
    return false;
}

// Kilo may replace an old, completed tool-result body with this marker
// without performing full conversation compaction. This does not authorize
// modifications of user instructions, tool-call IDs, or pending results.
inline bool is_pruned_tool_result(const json& expected, const json& received) {
    if (!expected.is_object() || !received.is_object() ||
        expected.value("role", "") != "tool" ||
        received.value("role", "") != "tool" ||
        expected.value("tool_call_id", "") != received.value("tool_call_id", "")) {
        return false;
    }
    const json body = canonical_content(received.value("content", json()));
    return body == "[Old tool result content cleared]" ||
           body == "[Old tool result content pruned]";
}

inline bool same_or_pruned(const json& previous, const json& incoming,
                          bool allow_prune) {
    if (history_messages_equivalent(canonical_history_message(previous),
                                    canonical_history_message(incoming))) return true;
    return allow_prune && is_pruned_tool_result(previous, incoming);
}

// A client may replace an old prefix with a summary and retain the latest
// complete tool/assistant exchange. Only explicit, stable hybrid session IDs
// may perform this rebase, and a unique, >=2-message exact suffix is required.
// Never replay condensed client text into the private role histories.
inline std::optional<size_t> retained_suffix_end(const json& previous,
                                                  const json& incoming) {
    if (!previous.is_array() || !incoming.is_array() || previous.size() < 3) {
        return std::nullopt;
    }
    size_t protected_count = 0;
    while (protected_count < previous.size()) {
        const std::string role = previous[protected_count].value("role", "");
        if (role != "system" && role != "developer") break;
        if (protected_count >= incoming.size() ||
            !same_or_pruned(previous[protected_count], incoming[protected_count], false)) {
            return std::nullopt;
        }
        ++protected_count;
    }
    // A pending assistant tool-call is a particularly strong suffix anchor.
    // Require at least two unchanged prior messages to distinguish sessions.
    std::optional<size_t> match;
    const size_t max_length = std::min<size_t>(32, previous.size() - protected_count);
    for (size_t length = max_length; length >= 2; --length) {
        const size_t old_start = previous.size() - length;
        if (incoming.size() <= length) continue;
        // Appended client tool results/user messages should be near the tail;
        // never scan a million-message transcript quadratically.
        const size_t earliest = incoming.size() > length + 64
            ? std::max(protected_count, incoming.size() - length - 64)
            : protected_count;
        for (size_t new_start = earliest; new_start + length < incoming.size(); ++new_start) {
            bool same = true;
            for (size_t j = 0; j < length; ++j) {
                if (!same_or_pruned(previous[old_start + j], incoming[new_start + j], false)) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            if (old_start == new_start && incoming.size() >= previous.size()) continue;
            const size_t new_end = new_start + length;
            if (match && *match != new_end) return std::nullopt;
            match = new_end;
        }
        if (length == 2) break;
    }
    return match;
}

inline std::string history_mismatch_field(const json& expected, const json& received) {
    if (!expected.is_object() || !received.is_object()) return "message structure";
    if (expected.value("role", "") != received.value("role", "")) return "role";
    if (expected.value("tool_call_id", json()) != received.value("tool_call_id", json())) {
        return "tool_call_id";
    }
    if (expected.value("tool_calls", json::array()) != received.value("tool_calls", json::array())) {
        return "tool calls (ID, function name or arguments)";
    }
    if (expected.value("content", json()) != received.value("content", json())) {
        return "content";
    }
    return "message structure";
}

} // namespace lemon::hybrid_history
