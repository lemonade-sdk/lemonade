#include "lemon/request_log_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace lemon {
namespace {

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool is_sensitive_key(const std::string& key) {
    static const char* const keys[] = {
        "authorization", "api_key", "token", "password", "cookie", "secret",
        "bearer", "access_token", "refresh_token",
    };
    for (const char* candidate : keys) {
        if (iequals(key, candidate)) {
            return true;
        }
    }
    return false;
}

std::string strip_api_prefix(const std::string& path) {
    static const char* const prefixes[] = {
        "/api/v0/", "/api/v1/", "/v0/", "/v1/",
    };
    for (const char* prefix : prefixes) {
        const size_t len = std::char_traits<char>::length(prefix);
        if (path.rfind(prefix, 0) == 0) {
            return path.substr(len);
        }
    }
    return path;
}

bool path_ends_with_static_asset(const std::string& path) {
    static const char* const suffixes[] = {
        ".js", ".css", ".svg", ".png", ".ico", ".woff", ".woff2", ".map",
    };
    for (const char* suffix : suffixes) {
        const size_t len = std::char_traits<char>::length(suffix);
        if (path.size() >= len &&
            path.compare(path.size() - len, len, suffix) == 0) {
            return true;
        }
    }
    return false;
}

int count_message_chars(const nlohmann::json& messages) {
    if (!messages.is_array()) {
        return 0;
    }
    int total = 0;
    for (const auto& message : messages) {
        if (message.is_object() && message.contains("content")) {
            const auto& content = message["content"];
            if (content.is_string()) {
                total += static_cast<int>(content.get<std::string>().size());
            } else if (content.is_array()) {
                for (const auto& part : content) {
                    if (part.is_object()) {
                        if (part.contains("text") && part["text"].is_string()) {
                            total += static_cast<int>(part["text"].get<std::string>().size());
                        } else if (part.contains("input_text") &&
                                   part["input_text"].is_string()) {
                            total += static_cast<int>(part["input_text"].get<std::string>().size());
                        }
                    }
                }
            }
        }
    }
    return total;
}

std::string json_value_to_string(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << value.get<double>();
        return oss.str();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return value.dump();
}

constexpr size_t kMaxRedactedBodyBytes = 32768;

bool is_valid_utf8(const std::string& value) {
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        if (byte <= 0x7F) {
            ++i;
            continue;
        }
        size_t extra = 0;
        if ((byte & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= value.size()) {
            return false;
        }
        for (size_t j = 1; j <= extra; ++j) {
            const unsigned char continuation =
                static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

bool looks_like_binary_payload(const std::string& value) {
    if (value.size() >= 2 && static_cast<unsigned char>(value[0]) == 0x1F &&
        static_cast<unsigned char>(value[1]) == 0x8B) {
        return true;
    }
    return !is_valid_utf8(value);
}

} // namespace

nlohmann::json redact_json(const nlohmann::json& value) {
    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (is_sensitive_key(it.key())) {
                out[it.key()] = "[REDACTED]";
            } else {
                out[it.key()] = redact_json(it.value());
            }
        }
        return out;
    }
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : value) {
            out.push_back(redact_json(item));
        }
        return out;
    }
    return value;
}

std::string classify_endpoint_type(const std::string& path, const std::string& method) {
    (void)method;
    if (path.rfind("/internal/", 0) == 0) {
        return "lemonade";
    }

    static const char* const ollama_paths[] = {
        "/api/chat", "/api/generate", "/api/tags", "/api/show", "/api/delete",
        "/api/pull", "/api/embed", "/api/embeddings", "/api/ps", "/api/version",
    };
    for (const char* ollama_path : ollama_paths) {
        if (path == ollama_path) {
            return "ollama";
        }
    }

    if (path == "/v1/messages" || path == "/api/v1/messages") {
        return "openai";
    }

    const std::string relative = strip_api_prefix(path);
    static const char* const openai_paths[] = {
        "chat/completions", "completions", "embeddings", "reranking", "responses",
        "audio/transcriptions", "audio/speech", "images/generations", "images/edits",
        "images/variations", "images/upscale",
    };
    for (const char* openai_path : openai_paths) {
        if (relative == openai_path) {
            return "openai";
        }
    }

    static const char* const lemonade_paths[] = {
        "load", "unload", "pull", "delete", "params", "install", "uninstall",
    };
    for (const char* lemonade_path : lemonade_paths) {
        if (relative == lemonade_path) {
            return "lemonade";
        }
    }

    if (path == "/mcp") {
        return "lemonade";
    }

    return "other";
}

std::string extract_forwarded_for(const std::string& x_forwarded_for,
                                  const std::string& x_real_ip,
                                  const std::string& forwarded) {
    if (!x_forwarded_for.empty()) {
        return x_forwarded_for;
    }
    if (!x_real_ip.empty()) {
        return x_real_ip;
    }
    return forwarded;
}

ParsedRequestBody parse_request_body(const std::string& body,
                                     const std::string& path,
                                     bool log_prompts) {
    ParsedRequestBody parsed;
    if (body.empty()) {
        return parsed;
    }

    nlohmann::json request_json;
    try {
        request_json = nlohmann::json::parse(body);
    } catch (...) {
        return parsed;
    }

    if (!request_json.is_object()) {
        return parsed;
    }

    if (request_json.contains("model") && request_json["model"].is_string()) {
        parsed.model = request_json["model"].get<std::string>();
    } else if (request_json.contains("model_name") &&
               request_json["model_name"].is_string()) {
        parsed.model = request_json["model_name"].get<std::string>();
    }

    if (request_json.contains("keep_alive")) {
        parsed.keep_alive = json_value_to_string(request_json["keep_alive"]);
    }

    if (request_json.contains("stream") && request_json["stream"].is_boolean()) {
        parsed.stream = request_json["stream"].get<bool>();
    }

    if (request_json.contains("prompt") && request_json["prompt"].is_string()) {
        parsed.prompt_chars =
            static_cast<int>(request_json["prompt"].get<std::string>().size());
    }

    if (request_json.contains("messages")) {
        parsed.messages_chars = count_message_chars(request_json["messages"]);
    }

    nlohmann::json redacted = redact_json(request_json);
    if (!log_prompts) {
        if (redacted.contains("prompt") && redacted["prompt"].is_string()) {
            redacted["prompt"] = nlohmann::json{{"char_count", parsed.prompt_chars}};
        }
        if (redacted.contains("messages") && redacted["messages"].is_array()) {
            redacted["messages"] = nlohmann::json{{"char_count", parsed.messages_chars}};
        }
    }

    const std::string relative = strip_api_prefix(path);
    if ((relative == "load" || path == "/internal/pin") &&
        request_json.contains("pinned") && request_json["pinned"].is_boolean()) {
        redacted["_meta"] = nlohmann::json{{"pinned", request_json["pinned"].get<bool>()}};
    }

    const std::string dumped = redacted.dump();
    if (dumped.size() <= kMaxRedactedBodyBytes) {
        parsed.redacted_body = std::move(redacted);
        parsed.has_redacted_body = true;
    } else {
        parsed.redacted_body = nlohmann::json{
            {"truncated", true},
            {"original_bytes", dumped.size()},
        };
        parsed.has_redacted_body = true;
    }

    return parsed;
}

std::string sanitize_utf8_for_db(std::string value);

std::optional<int> json_int_field(const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key)) {
        return std::nullopt;
    }
    const auto& value = obj[key];
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned>());
    }
    return std::nullopt;
}

std::string extract_response_text(const nlohmann::json& response_json) {
    if (response_json.contains("choices") && response_json["choices"].is_array() &&
        !response_json["choices"].empty()) {
        const auto& choice = response_json["choices"][0];
        if (choice.is_object()) {
            if (choice.contains("message") && choice["message"].is_object() &&
                choice["message"].contains("content")) {
                return json_value_to_string(choice["message"]["content"]);
            }
            if (choice.contains("text") && choice["text"].is_string()) {
                return choice["text"].get<std::string>();
            }
        }
    }
    if (response_json.contains("message") && response_json["message"].is_object() &&
        response_json["message"].contains("content")) {
        return json_value_to_string(response_json["message"]["content"]);
    }
    if (response_json.contains("response") && response_json["response"].is_string()) {
        return response_json["response"].get<std::string>();
    }
    return {};
}

ParsedResponseBody parse_response_body(const std::string& body,
                                       const std::string& path,
                                       int status_code,
                                       bool log_prompts) {
    (void)path;
    ParsedResponseBody parsed;
    if (body.empty()) {
        return parsed;
    }

    nlohmann::json response_json;
    try {
        response_json = nlohmann::json::parse(body);
    } catch (...) {
        if (status_code >= 200 && status_code < 300) {
            const std::string preview =
                body.size() > 512 ? body.substr(0, 512) : body;
            parsed.redacted_response = nlohmann::json{
                {"note", "non-JSON response"},
                {"preview", sanitize_utf8_for_db(preview)},
            };
            parsed.has_redacted_response = true;
        }
        return parsed;
    }

    if (!response_json.is_object()) {
        return parsed;
    }

    if (response_json.contains("usage") && response_json["usage"].is_object()) {
        const auto& usage = response_json["usage"];
        parsed.prompt_tokens = json_int_field(usage, "prompt_tokens");
        parsed.completion_tokens = json_int_field(usage, "completion_tokens");
    }
    if (!parsed.prompt_tokens.has_value()) {
        parsed.prompt_tokens = json_int_field(response_json, "prompt_eval_count");
    }
    if (!parsed.completion_tokens.has_value()) {
        parsed.completion_tokens = json_int_field(response_json, "eval_count");
    }

    nlohmann::json summary = nlohmann::json::object();
    if (parsed.prompt_tokens.has_value()) {
        summary["prompt_tokens"] = parsed.prompt_tokens.value();
    }
    if (parsed.completion_tokens.has_value()) {
        summary["completion_tokens"] = parsed.completion_tokens.value();
    }

    const std::string content = extract_response_text(response_json);
    if (!content.empty()) {
        if (log_prompts) {
            summary["content"] = content;
        } else {
            summary["content"] = nlohmann::json{{"char_count", static_cast<int>(content.size())}};
        }
    }

    if (log_prompts) {
        summary["body"] = redact_json(response_json);
    } else if (status_code >= 400) {
        summary["body"] = redact_json(response_json);
    }

    if (!summary.empty()) {
        const std::string dumped = summary.dump();
        if (dumped.size() <= kMaxRedactedBodyBytes) {
            parsed.redacted_response = std::move(summary);
            parsed.has_redacted_response = true;
        } else {
            parsed.redacted_response = nlohmann::json{
                {"truncated", true},
                {"original_bytes", dumped.size()},
                {"prompt_tokens", parsed.prompt_tokens.value_or(0)},
                {"completion_tokens", parsed.completion_tokens.value_or(0)},
            };
            parsed.has_redacted_response = true;
        }
    }

    return parsed;
}

std::string sanitize_utf8_for_db(std::string value) {
    if (value.empty() || is_valid_utf8(value)) {
        return value;
    }

    std::string sanitized;
    sanitized.reserve(value.size());
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char byte = static_cast<unsigned char>(value[i]);
        size_t seq_len = 1;
        bool valid = true;
        if (byte <= 0x7F) {
            seq_len = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            seq_len = 4;
        } else {
            valid = false;
        }

        if (valid && i + seq_len <= value.size()) {
            for (size_t j = 1; j < seq_len; ++j) {
                const unsigned char continuation =
                    static_cast<unsigned char>(value[i + j]);
                if ((continuation & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
        } else {
            valid = false;
        }

        if (valid) {
            sanitized.append(value, i, seq_len);
            i += seq_len;
        } else {
            sanitized.append("\xEF\xBF\xBD", 3);
            ++i;
        }
    }
    return sanitized;
}

std::string extract_response_error(const std::string& response_body, int status_code) {
    if (status_code >= 200 && status_code < 300) {
        return {};
    }
    if (response_body.empty()) {
        return {};
    }
    try {
        auto response_json = nlohmann::json::parse(response_body);
        if (response_json.is_object()) {
            if (response_json.contains("error")) {
                const auto& error = response_json["error"];
                if (error.is_string()) {
                    return sanitize_utf8_for_db(error.get<std::string>());
                }
                if (error.is_object() && error.contains("message") &&
                    error["message"].is_string()) {
                    return sanitize_utf8_for_db(error["message"].get<std::string>());
                }
                return sanitize_utf8_for_db(error.dump());
            }
            if (response_json.contains("message") &&
                response_json["message"].is_string()) {
                return sanitize_utf8_for_db(response_json["message"].get<std::string>());
            }
        }
    } catch (...) {
    }
    if (looks_like_binary_payload(response_body)) {
        return "[non-UTF-8 response body, " + std::to_string(response_body.size()) + " bytes]";
    }
    if (response_body.size() > 512) {
        return sanitize_utf8_for_db(response_body.substr(0, 512));
    }
    return sanitize_utf8_for_db(response_body);
}

bool should_skip_request_log_path(const std::string& path, const std::string& method) {
    if (path == "/api/v0/health" || path == "/api/v1/health" ||
        path == "/v0/health" || path == "/v1/health" ||
        path == "/live" || path == "/metrics") {
        return true;
    }

    if (path == "/api/v0/downloads" || path == "/api/v1/downloads" ||
        path == "/v0/downloads" || path == "/v1/downloads" ||
        path == "/api/v0/system-stats" || path == "/api/v1/system-stats" ||
        path == "/v0/system-stats" || path == "/v1/system-stats" ||
        path == "/api/v0/stats" || path == "/api/v1/stats" ||
        path == "/v0/stats" || path == "/v1/stats") {
        return true;
    }

    if (path.find("request-log/") != std::string::npos) {
        return true;
    }

    if (method == "GET" && (path == "/" || path == "/app" || path.rfind("/app/", 0) == 0 ||
                            path.rfind("/static/", 0) == 0 || path_ends_with_static_asset(path))) {
        return true;
    }

    if (method == "GET" && (
            path == "/api/v0/models" || path == "/api/v1/models" ||
            path == "/v0/models" || path == "/v1/models" ||
            path == "/api/v0/system-info" || path == "/api/v1/system-info" ||
            path == "/v0/system-info" || path == "/v1/system-info" ||
            path == "/api/v0/system-checks" || path == "/api/v1/system-checks" ||
            path == "/v0/system-checks" || path == "/v1/system-checks")) {
        return true;
    }

    return false;
}

} // namespace lemon
