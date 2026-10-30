#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace lemon {
namespace utils {

// 64-bit FNV-1a fingerprint of a conversation's stable prefix (system text
// plus the first user message text). Used as a metrics key for route-stability
// counters only — it never influences routing and no message content is
// retained alongside it. Later turns append to the conversation, so the
// fingerprint stays constant for the lifetime of one conversation.

inline void fingerprint_append(uint64_t& hash, const std::string& text) {
    for (unsigned char c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
}

inline void fingerprint_append_content(uint64_t& hash, const nlohmann::json& content) {
    if (content.is_string()) {
        fingerprint_append(hash, content.get<std::string>());
    } else if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_string()) {
                fingerprint_append(hash, part.get<std::string>());
            } else if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                fingerprint_append(hash, part["text"].get<std::string>());
            }
        }
    }
}

inline uint64_t conversation_fingerprint(const nlohmann::json& request_json) {
    uint64_t hash = 14695981039346656037ULL;

    const nlohmann::json* messages = nullptr;
    if (request_json.contains("messages") && request_json["messages"].is_array()) {
        messages = &request_json["messages"];
    } else if (request_json.contains("input") && request_json["input"].is_array()) {
        messages = &request_json["input"];
    }

    if (messages) {
        for (const auto& message : *messages) {
            if (message.is_object() && message.value("role", "") == "system" &&
                message.contains("content")) {
                fingerprint_append_content(hash, message["content"]);
                break;
            }
        }
        fingerprint_append(hash, "\x1f");
        for (const auto& message : *messages) {
            if (message.is_object() && message.value("role", "") == "user" &&
                message.contains("content")) {
                fingerprint_append_content(hash, message["content"]);
                break;
            }
        }
        return hash;
    }

    if (request_json.contains("prompt")) {
        fingerprint_append(hash, "\x1f");
        fingerprint_append_content(hash, request_json["prompt"]);
    } else if (request_json.contains("input") && request_json["input"].is_string()) {
        fingerprint_append(hash, "\x1f");
        fingerprint_append(hash, request_json["input"].get<std::string>());
    }
    return hash;
}

} // namespace utils
} // namespace lemon
