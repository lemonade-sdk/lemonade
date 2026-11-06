#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <mbedtls/md.h>
#include <nlohmann/json.hpp>

// The frames lemond and lemonade-agent-host exchange over the host's
// WebSocket. Every frame is a JSON text message with a "type".
namespace lemon::agents::protocol {

constexpr int kVersion = 1;
constexpr const char* kEndpoint = "/v1/agents/host";
constexpr const char* kTokenHeader = "X-Lemonade-Agent-Token";

constexpr uint16_t kCloseBadHello = 4400;
constexpr uint16_t kCloseHelloTimeout = 4408;
constexpr uint16_t kCloseProtocolMismatch = 4409;
constexpr uint16_t kCloseTooManyHosts = 4429;

inline bool is_token(const std::string& token) {
    if (token.size() != 64) {
        return false;
    }
    for (char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// lemond keeps only this, so a token is never stored or logged.
inline std::string token_hash(const std::string& token) {
    unsigned char digest[32] = {};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr ||
        mbedtls_md(info, reinterpret_cast<const unsigned char*>(token.data()), token.size(),
                   digest) != 0) {
        return "";
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xf]);
    }
    return out;
}

inline bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

inline nlohmann::json hello(const std::string& token, const std::string& version,
                            const std::string& platform, bool persistent) {
    return {{"type", "hello"}, {"protocol", kVersion}, {"token", token},
            {"version", version}, {"platform", platform}, {"persistent", persistent}};
}

inline nlohmann::json welcome(bool auth_required) {
    return {{"type", "welcome"}, {"auth_required", auth_required}};
}

inline nlohmann::json request(uint64_t id, const std::string& route, const nlohmann::json& params,
                              const std::string& token, const nlohmann::json& body) {
    return {{"type", "request"}, {"id", id}, {"route", route}, {"params", params},
            {"token", token}, {"body", body}};
}

inline nlohmann::json cancel(uint64_t id) {
    return {{"type", "cancel"}, {"id", id}};
}

inline nlohmann::json event(uint64_t id, const std::string& name, const nlohmann::json& data) {
    return {{"type", "event"}, {"id", id}, {"event", name}, {"data", data}};
}

inline nlohmann::json response(uint64_t id, int status, const nlohmann::json& body) {
    return {{"type", "response"}, {"id", id}, {"status", status}, {"body", body}};
}

// lemond's plain error shape. As an SSE error event's data it also carries "status".
inline nlohmann::json error_body(const std::string& code, const std::string& message,
                                 const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json body = extra.is_object() ? extra : nlohmann::json::object();
    body["error"] = message;
    body["code"] = code;
    return body;
}

// Returns the frame when text is a well-formed frame of a known type, else nullopt.
inline std::optional<nlohmann::json> parse_frame(const std::string& text) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        return std::nullopt;
    }
    const std::string type = j["type"];
    auto has = [&j](const char* key, bool (nlohmann::json::*is)() const noexcept) {
        return j.contains(key) && (j[key].*is)();
    };
    const bool has_id = has("id", &nlohmann::json::is_number_unsigned);
    bool ok = false;
    if (type == "hello") {
        ok = has("protocol", &nlohmann::json::is_number_integer) &&
             has("token", &nlohmann::json::is_string) &&
             has("version", &nlohmann::json::is_string) &&
             has("platform", &nlohmann::json::is_string) &&
             has("persistent", &nlohmann::json::is_boolean);
    } else if (type == "welcome") {
        ok = has("auth_required", &nlohmann::json::is_boolean);
    } else if (type == "request") {
        ok = has_id && has("route", &nlohmann::json::is_string) &&
             has("params", &nlohmann::json::is_object) &&
             has("token", &nlohmann::json::is_string) && j.contains("body");
    } else if (type == "cancel") {
        ok = has_id;
    } else if (type == "event") {
        ok = has_id && has("event", &nlohmann::json::is_string) && j.contains("data");
    } else if (type == "response") {
        ok = has_id && has("status", &nlohmann::json::is_number_integer) && j.contains("body");
    }
    if (!ok) {
        return std::nullopt;
    }
    return j;
}

} // namespace lemon::agents::protocol
