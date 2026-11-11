#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace lemon {
namespace openai {

// The "content" of a chat message is optional on the wire: a reasoning model
// whose output budget is exhausted inside its thinking can answer with
// content: null instead of content: "". Reading that field unguarded
// (.get<std::string>()) throws nlohmann type_error.302. Return the content
// only when it is actually a string.
inline std::optional<std::string> message_content(const nlohmann::json& message) {
    if (message.is_object() && message.contains("content") && message["content"].is_string()) {
        return message["content"].get<std::string>();
    }
    return std::nullopt;
}

} // namespace openai
} // namespace lemon
