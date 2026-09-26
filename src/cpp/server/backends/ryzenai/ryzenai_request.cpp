#include "lemon/backends/ryzenai/ryzenai_request.h"
#include "lemon/error_types.h"
#include <string>

namespace lemon {
namespace ryzenai {

nlohmann::json normalize_chat_request(const nlohmann::json& request) {
    nlohmann::json normalized = request;
    if (!normalized.contains("messages") || !normalized["messages"].is_array()) {
        return normalized;
    }

    for (size_t i = 0; i < normalized["messages"].size(); ++i) {
        auto& msg = normalized["messages"][i];
        if (!msg.contains("content")) continue;

        auto& content = msg["content"];
        if (content.is_string() || content.is_null()) {
            continue;
        }

        if (content.is_array()) {
            std::string combined_text = "";
            for (size_t j = 0; j < content.size(); ++j) {
                const auto& part = content[j];
                if (!part.is_object()) {
                    throw InvalidRequestException("RyzenAI supports only text message content; messages[" + std::to_string(i) + "].content[" + std::to_string(j) + "] is not an object");
                }
                if (!part.contains("type")) {
                    throw InvalidRequestException("RyzenAI supports only text message content; messages[" + std::to_string(i) + "].content[" + std::to_string(j) + "] is missing 'type'");
                }
                if (part["type"] != "text") {
                    throw InvalidRequestException("RyzenAI supports only text message content; messages[" + std::to_string(i) + "].content[" + std::to_string(j) + "] has unsupported type '" + part.value("type", "") + "'");
                }
                if (!part.contains("text")) {
                    throw InvalidRequestException("RyzenAI supports only text message content; messages[" + std::to_string(i) + "].content[" + std::to_string(j) + "] is missing 'text'");
                }
                if (!part["text"].is_string()) {
                    throw InvalidRequestException("RyzenAI supports only text message content; messages[" + std::to_string(i) + "].content[" + std::to_string(j) + "] 'text' is not a string");
                }
                combined_text += part["text"].get<std::string>();
            }
            msg["content"] = combined_text;
        }
    }

    return normalized;
}

} // namespace ryzenai
} // namespace lemon
