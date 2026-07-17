#include "lemon/ollama_api.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace lemon {

namespace {

static void add_warning(std::vector<std::string>& warnings, const std::string& warning) {
    if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
        warnings.push_back(warning);
    }
}

static std::string join_strings(const std::vector<std::string>& parts, const char* sep = "") {
    std::ostringstream os;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) os << sep;
        os << parts[i];
    }
    return os.str();
}

static void append_joined_text(std::string& target, const std::string& value, const char* sep = "") {
    if (value.empty()) {
        return;
    }
    if (!target.empty()) {
        target += sep;
    }
    target += value;
}

static std::string trim_ascii_whitespace(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

struct SplitThinkingText {
    std::string thinking;
    std::string text;
    bool found_thinking = false;
};

static SplitThinkingText split_inline_thinking_tags(const std::string& content) {
    static const std::string open_tag = "<think>";
    static const std::string close_tag = "</think>";

    SplitThinkingText result;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t open_pos = content.find(open_tag, pos);
        if (open_pos == std::string::npos) {
            result.text += content.substr(pos);
            break;
        }

        result.found_thinking = true;
        result.text += content.substr(pos, open_pos - pos);

        size_t thinking_begin = open_pos + open_tag.size();
        size_t close_pos = content.find(close_tag, thinking_begin);
        if (close_pos == std::string::npos) {
            result.thinking += content.substr(thinking_begin);
            break;
        }

        append_joined_text(result.thinking,
                           content.substr(thinking_begin, close_pos - thinking_begin));
        pos = close_pos + close_tag.size();
    }

    if (result.found_thinking) {
        result.thinking = trim_ascii_whitespace(result.thinking);
        result.text = trim_ascii_whitespace(result.text);
    }

    return result;
}

static size_t tag_prefix_suffix_length(const std::string& value, const std::string& tag) {
    if (value.empty() || tag.empty()) {
        return 0;
    }

    size_t max_len = std::min(value.size(), tag.size() - 1);
    for (size_t len = max_len; len > 0; --len) {
        if (value.compare(value.size() - len, len, tag, 0, len) == 0) {
            return len;
        }
    }
    return 0;
}

static json make_anthropic_thinking_block(const std::string& thinking) {
    return {
        {"type", "thinking"},
        {"thinking", thinking},
        {"signature", ""}
    };
}

static bool valid_http_error_status(int status_code) {
    return status_code >= 400 && status_code <= 599;
}

static int get_backend_error_status(const json& error, int default_status_code = 500) {
    if (!error.is_object()) {
        return default_status_code;
    }

    if (error.contains("status_code") && error["status_code"].is_number_integer()) {
        int status_code = error["status_code"].get<int>();
        if (valid_http_error_status(status_code)) {
            return status_code;
        }
    }

    if (error.contains("details") && error["details"].is_object()) {
        const auto& details = error["details"];
        if (details.contains("status_code") && details["status_code"].is_number_integer()) {
            int status_code = details["status_code"].get<int>();
            if (valid_http_error_status(status_code)) {
                return status_code;
            }
        }
    }

    if (error.contains("type") && error["type"].is_string()) {
        const std::string type = error["type"].get<std::string>();
        if (type == "invalid_request" || type == "unsupported_operation") {
            return 400;
        }
        if (type == "model_not_loaded") {
            return 404;
        }
        if (type == "rate_limit_error") {
            return 429;
        }
    }

    return default_status_code;
}

static std::string map_status_to_anthropic_error_type(int status_code) {
    if (status_code == 401) {
        return "authentication_error";
    }
    if (status_code == 402) {
        return "billing_error";
    }
    if (status_code == 403) {
        return "permission_error";
    }
    if (status_code == 404) {
        return "not_found_error";
    }
    if (status_code == 409) {
        return "conflict_error";
    }
    if (status_code == 413) {
        return "request_too_large";
    }
    if (status_code == 429) {
        return "rate_limit_error";
    }
    if (status_code == 504) {
        return "timeout_error";
    }
    if (status_code == 529) {
        return "overloaded_error";
    }
    if (status_code >= 400 && status_code < 500) {
        return "invalid_request_error";
    }
    return "api_error";
}

static std::string get_backend_error_message(const json& error) {
    if (error.is_object()) {
        if (error.contains("message") && error["message"].is_string()) {
            return error["message"].get<std::string>();
        }
        return error.dump();
    }
    if (error.is_string()) {
        return error.get<std::string>();
    }
    if (!error.is_null()) {
        return error.dump();
    }
    return "backend error";
}

static json make_anthropic_error(const json& error, int status_code) {
    return {
        {"type", "error"},
        {"error", {
            {"type", map_status_to_anthropic_error_type(status_code)},
            {"message", get_backend_error_message(error)}
        }}
    };
}

static bool send_anthropic_backend_error(const json& response, httplib::Response& res) {
    if (!response.contains("error")) {
        return false;
    }

    const auto& error = response["error"];
    int status_code = get_backend_error_status(error);
    res.status = status_code;
    res.set_content(make_anthropic_error(error, status_code).dump(), "application/json");
    return true;
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

static std::string map_finish_reason_to_anthropic_stop_reason(
    const json& choice,
    std::vector<std::string>* warnings = nullptr) {
    if (!choice.contains("finish_reason") || choice["finish_reason"].is_null()) {
        return "end_turn";
    }
    if (!choice["finish_reason"].is_string()) {
        if (warnings) {
            add_warning(*warnings, "Backend returned a non-string finish_reason; mapped to end_turn");
        }
        return "end_turn";
    }

    const std::string finish_reason = choice["finish_reason"].get<std::string>();
    if (finish_reason == "length" || finish_reason == "max_tokens") {
        return "max_tokens";
    }
    if (finish_reason == "tool_calls" || finish_reason == "tool_use") {
        return "tool_use";
    }
    if (finish_reason == "stop" || finish_reason == "end_turn") {
        return "end_turn";
    }
    if (finish_reason == "stop_sequence" || finish_reason == "refusal" ||
        finish_reason == "pause_turn") {
        return finish_reason;
    }
    if (finish_reason == "content_filter") {
        return "refusal";
    }
    if (warnings) {
        add_warning(*warnings, "Unknown backend finish_reason '" + finish_reason + "' mapped to end_turn");
    }
    return "end_turn";
}

static std::string generate_anthropic_id(const char* prefix) {
    static std::atomic<uint64_t> sequence{
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    return std::string(prefix) + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

static std::string generate_anthropic_message_id() {
    return generate_anthropic_id("msg_");
}

static std::string generate_anthropic_tool_id() {
    return generate_anthropic_id("toolu_");
}

static std::string warning_header_value(const std::vector<std::string>& warnings) {
    std::string value;
    for (const auto& warning : warnings) {
        if (!value.empty()) {
            value += " | ";
        }
        for (unsigned char c : warning) {
            value += (c >= 0x20 && c != 0x7f) ? static_cast<char>(c) : '?';
        }
        if (value.size() >= 4096) {
            value.resize(4096);
            break;
        }
    }
    return value;
}

struct AnthropicUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_creation_input_tokens = 0;
    int cache_read_input_tokens = 0;
    bool has_cache_creation = false;
    bool has_cache_read = false;
};

static int nonnegative_int(const json& object, const char* key, int fallback = 0) {
    if (!object.contains(key) || !object[key].is_number_integer()) {
        return fallback;
    }
    return std::max(0, object[key].get<int>());
}

static AnthropicUsage convert_openai_usage(const json& usage) {
    AnthropicUsage result;
    if (!usage.is_object()) {
        return result;
    }

    result.output_tokens = nonnegative_int(
        usage, "completion_tokens", nonnegative_int(usage, "output_tokens"));
    result.cache_creation_input_tokens = nonnegative_int(usage, "cache_creation_input_tokens");
    result.cache_read_input_tokens = nonnegative_int(usage, "cache_read_input_tokens");
    result.has_cache_creation = usage.contains("cache_creation_input_tokens");
    result.has_cache_read = usage.contains("cache_read_input_tokens");

    if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
        const auto& details = usage["prompt_tokens_details"];
        if (details.contains("cached_tokens")) {
            result.cache_read_input_tokens = nonnegative_int(details, "cached_tokens");
            result.has_cache_read = true;
        }
    }
    if (usage.contains("input_tokens_details") && usage["input_tokens_details"].is_object()) {
        const auto& details = usage["input_tokens_details"];
        if (details.contains("cached_tokens")) {
            result.cache_read_input_tokens = nonnegative_int(details, "cached_tokens");
            result.has_cache_read = true;
        }
    }

    const int total_input = nonnegative_int(
        usage, "prompt_tokens", nonnegative_int(usage, "input_tokens"));
    result.input_tokens = std::max(
        0, total_input - result.cache_creation_input_tokens - result.cache_read_input_tokens);
    return result;
}

static json anthropic_usage_json(const AnthropicUsage& usage, bool include_input) {
    json result = {{"output_tokens", usage.output_tokens}};
    if (include_input) {
        result["input_tokens"] = usage.input_tokens;
    }
    if (usage.has_cache_creation) {
        result["cache_creation_input_tokens"] = usage.cache_creation_input_tokens;
    }
    if (usage.has_cache_read) {
        result["cache_read_input_tokens"] = usage.cache_read_input_tokens;
    }
    return result;
}

static json extract_stop_sequence(const json& response, const json& choice) {
    for (const auto* key : {"stop_sequence", "stopping_word"}) {
        if (choice.contains(key) && choice[key].is_string() && !choice[key].get<std::string>().empty()) {
            return choice[key];
        }
        if (response.contains(key) && response[key].is_string() && !response[key].get<std::string>().empty()) {
            return response[key];
        }
    }
    if (response.contains("__verbose") && response["__verbose"].is_object()) {
        const auto& verbose = response["__verbose"];
        if (verbose.contains("stopping_word") && verbose["stopping_word"].is_string() &&
            !verbose["stopping_word"].get<std::string>().empty()) {
            return verbose["stopping_word"];
        }
    }
    return nullptr;
}

static void validate_anthropic_request(const json& request, bool count_tokens) {
    if (!request.is_object()) {
        throw std::invalid_argument("request body must be a JSON object");
    }
    if (!request.contains("model") || !request["model"].is_string() ||
        request["model"].get<std::string>().empty()) {
        throw std::invalid_argument("model is required and must be a non-empty string");
    }
    if (!request.contains("messages") || !request["messages"].is_array()) {
        throw std::invalid_argument("messages is required and must be an array");
    }
    if (request["messages"].empty()) {
        throw std::invalid_argument("messages must contain at least one message");
    }
    if (!count_tokens) {
        if (!request.contains("max_tokens") || !request["max_tokens"].is_number_integer() ||
            request["max_tokens"].get<int64_t>() < 0) {
            throw std::invalid_argument("max_tokens is required and must be a non-negative integer");
        }
    }
    if (request.contains("stream") && !request["stream"].is_boolean()) {
        throw std::invalid_argument("stream must be a boolean");
    }
    for (const auto* key : {"temperature", "top_p"}) {
        if (request.contains(key) &&
            (!request[key].is_number() || request[key].get<double>() < 0.0 ||
             request[key].get<double>() > 1.0)) {
            throw std::invalid_argument(std::string(key) + " must be a number between 0 and 1");
        }
    }
    if (request.contains("top_k") &&
        (!request["top_k"].is_number_integer() || request["top_k"].get<int64_t>() < 0)) {
        throw std::invalid_argument("top_k must be a non-negative integer");
    }
    if (request.contains("stop_sequences")) {
        if (!request["stop_sequences"].is_array()) {
            throw std::invalid_argument("stop_sequences must be an array of strings");
        }
        for (const auto& stop : request["stop_sequences"]) {
            if (!stop.is_string()) {
                throw std::invalid_argument("stop_sequences must contain only strings");
            }
        }
    }

    auto validate_image_source = [](const json& block, const std::string& path) {
        if (!block.contains("source") || !block["source"].is_object()) {
            throw std::invalid_argument(path + ".source must be an object");
        }
        const auto& source = block["source"];
        if (!source.contains("type") || !source["type"].is_string()) {
            throw std::invalid_argument(path + ".source.type must be a string");
        }
        const std::string type = source["type"].get<std::string>();
        if (type == "base64") {
            if (!source.contains("media_type") || !source["media_type"].is_string() ||
                !source.contains("data") || !source["data"].is_string()) {
                throw std::invalid_argument(path + ".source requires string media_type and data");
            }
        } else if (type == "url") {
            if (!source.contains("url") || !source["url"].is_string() ||
                source["url"].get<std::string>().empty()) {
                throw std::invalid_argument(path + ".source.url must be a non-empty string");
            }
        } else {
            throw std::invalid_argument(path + ".source.type must be 'base64' or 'url'");
        }
    };

    auto validate_content = [&](const json& content, const std::string& role, const std::string& path) {
        if (content.is_string()) {
            return;
        }
        if (!content.is_array()) {
            throw std::invalid_argument(path + " must be a string or an array");
        }
        for (size_t i = 0; i < content.size(); ++i) {
            const auto& block = content[i];
            const std::string block_path = path + "[" + std::to_string(i) + "]";
            if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
                throw std::invalid_argument(block_path + " must be an object with a string type");
            }
            const std::string type = block["type"].get<std::string>();
            if (type == "text") {
                if (!block.contains("text") || !block["text"].is_string()) {
                    throw std::invalid_argument(block_path + ".text must be a string");
                }
            } else if (type == "image") {
                if (role != "user") {
                    throw std::invalid_argument("image blocks are only valid in user messages");
                }
                validate_image_source(block, block_path);
            } else if (type == "tool_use") {
                if (role != "assistant") {
                    throw std::invalid_argument("tool_use blocks are only valid in assistant messages");
                }
                if (!block.contains("id") || !block["id"].is_string() ||
                    !block.contains("name") || !block["name"].is_string() ||
                    !block.contains("input") || !block["input"].is_object()) {
                    throw std::invalid_argument(block_path + " requires string id/name and object input");
                }
                if (block["id"].get<std::string>().empty() || block["name"].get<std::string>().empty()) {
                    throw std::invalid_argument(block_path + " requires non-empty id and name");
                }
            } else if (type == "tool_result") {
                if (role != "user") {
                    throw std::invalid_argument("tool_result blocks are only valid in user messages");
                }
                if (!block.contains("tool_use_id") || !block["tool_use_id"].is_string()) {
                    throw std::invalid_argument(block_path + ".tool_use_id must be a string");
                }
                if (block["tool_use_id"].get<std::string>().empty()) {
                    throw std::invalid_argument(block_path + ".tool_use_id must be non-empty");
                }
                if (block.contains("is_error") && !block["is_error"].is_boolean()) {
                    throw std::invalid_argument(block_path + ".is_error must be a boolean");
                }
                if (block.contains("content")) {
                    const auto& result = block["content"];
                    if (!result.is_string() && !result.is_array()) {
                        throw std::invalid_argument(block_path + ".content must be a string or an array");
                    }
                    if (result.is_array()) {
                        for (size_t j = 0; j < result.size(); ++j) {
                            const auto& result_block = result[j];
                            const std::string result_path = block_path + ".content[" + std::to_string(j) + "]";
                            if (!result_block.is_object() || !result_block.contains("type") ||
                                !result_block["type"].is_string()) {
                                throw std::invalid_argument(result_path + " must have a string type");
                            }
                            const std::string result_type = result_block["type"].get<std::string>();
                            if (result_type == "text") {
                                if (!result_block.contains("text") || !result_block["text"].is_string()) {
                                    throw std::invalid_argument(result_path + ".text must be a string");
                                }
                            } else if (result_type == "image") {
                                validate_image_source(result_block, result_path);
                            } else {
                                throw std::invalid_argument(result_path + ".type is unsupported");
                            }
                        }
                    }
                }
            } else if (type == "thinking") {
                if (role != "assistant" || !block.contains("thinking") || !block["thinking"].is_string()) {
                    throw std::invalid_argument(block_path + " is not a valid assistant thinking block");
                }
                if (block.contains("signature") && !block["signature"].is_string()) {
                    throw std::invalid_argument(block_path + ".signature must be a string");
                }
            } else if (type == "redacted_thinking") {
                if (role != "assistant" || !block.contains("data") || !block["data"].is_string()) {
                    throw std::invalid_argument(block_path + " is not a valid assistant redacted_thinking block");
                }
            } else {
                throw std::invalid_argument(block_path + ".type '" + type + "' is unsupported");
            }
        }
    };

    for (size_t i = 0; i < request["messages"].size(); ++i) {
        const auto& message = request["messages"][i];
        const std::string path = "messages[" + std::to_string(i) + "]";
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string()) {
            throw std::invalid_argument(path + ".role must be 'user' or 'assistant'");
        }
        const std::string role = message["role"].get<std::string>();
        if (role != "user" && role != "assistant") {
            throw std::invalid_argument(path + ".role must be 'user' or 'assistant'");
        }
        if (!message.contains("content")) {
            throw std::invalid_argument(path + ".content is required");
        }
        validate_content(message["content"], role, path + ".content");
    }

    if (request.contains("system")) {
        const auto& system = request["system"];
        if (!system.is_string() && !system.is_array()) {
            throw std::invalid_argument("system must be a string or an array of text blocks");
        }
        if (system.is_array()) {
            for (size_t i = 0; i < system.size(); ++i) {
                const auto& block = system[i];
                if (!block.is_object() || !block.contains("type") || !block["type"].is_string() ||
                    block["type"].get<std::string>() != "text" ||
                    !block.contains("text") || !block["text"].is_string()) {
                    throw std::invalid_argument("system blocks must be text blocks with string text");
                }
            }
        }
    }

    if (request.contains("tools")) {
        if (!request["tools"].is_array()) {
            throw std::invalid_argument("tools must be an array");
        }
        std::unordered_set<std::string> tool_names;
        for (size_t i = 0; i < request["tools"].size(); ++i) {
            const auto& tool = request["tools"][i];
            if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string() ||
                tool["name"].get<std::string>().empty() || !tool.contains("input_schema") ||
                !tool["input_schema"].is_object()) {
                throw std::invalid_argument("tools[" + std::to_string(i) + "] requires a non-empty name and object input_schema");
            }
            if (tool.contains("description") && !tool["description"].is_string()) {
                throw std::invalid_argument("tools[" + std::to_string(i) + "].description must be a string");
            }
            if (!tool_names.insert(tool["name"].get<std::string>()).second) {
                throw std::invalid_argument("tool names must be unique");
            }
        }
    }

    if (request.contains("tool_choice")) {
        if (!request["tool_choice"].is_object() || !request["tool_choice"].contains("type") ||
            !request["tool_choice"]["type"].is_string()) {
            throw std::invalid_argument("tool_choice must be an object with a string type");
        }
        const std::string type = request["tool_choice"]["type"].get<std::string>();
        if (type != "auto" && type != "any" && type != "none" && type != "tool") {
            throw std::invalid_argument("tool_choice.type must be auto, any, none, or tool");
        }
        if (type != "none" && (!request.contains("tools") || request["tools"].empty())) {
            throw std::invalid_argument("tool_choice requires at least one tool");
        }
        if (type == "tool") {
            if (!request["tool_choice"].contains("name") || !request["tool_choice"]["name"].is_string()) {
                throw std::invalid_argument("tool_choice.type=tool requires a string name");
            }
            const std::string name = request["tool_choice"]["name"].get<std::string>();
            bool found = false;
            for (const auto& tool : request["tools"]) {
                found = found || tool.value("name", std::string()) == name;
            }
            if (!found) {
                throw std::invalid_argument("tool_choice.name must match a declared tool");
            }
        }
        if (request["tool_choice"].contains("disable_parallel_tool_use") &&
            !request["tool_choice"]["disable_parallel_tool_use"].is_boolean()) {
            throw std::invalid_argument("tool_choice.disable_parallel_tool_use must be a boolean");
        }
    }

    if (request.contains("thinking")) {
        if (!request["thinking"].is_object() || !request["thinking"].contains("type") ||
            !request["thinking"]["type"].is_string()) {
            throw std::invalid_argument("thinking must be an object with a string type");
        }
        const std::string type = request["thinking"]["type"].get<std::string>();
        if (type != "enabled" && type != "disabled" && type != "adaptive") {
            throw std::invalid_argument("thinking.type must be enabled, disabled, or adaptive");
        }
        if (type == "enabled" &&
            (!request["thinking"].contains("budget_tokens") ||
             !request["thinking"]["budget_tokens"].is_number_integer() ||
             request["thinking"]["budget_tokens"].get<int64_t>() <= 0)) {
            throw std::invalid_argument("thinking.budget_tokens must be a positive integer when thinking is enabled");
        }
        if ((type == "enabled" || type == "adaptive") && request.contains("tool_choice")) {
            const std::string tool_type = request["tool_choice"].value("type", std::string("auto"));
            if (tool_type == "any" || tool_type == "tool") {
                throw std::invalid_argument("thinking is incompatible with forced tool_choice modes");
            }
        }
    }
}

static bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string payload = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(payload.c_str(), payload.size());
}

static json convert_anthropic_tool_result_content(const json& content) {
    if (content.is_string()) {
        return content;
    }

    if (content.is_array()) {
        std::string text;
        json parts = json::array();
        bool has_images = false;
        for (const auto& block : content) {
            if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
                continue;
            }
            const std::string type = block["type"].get<std::string>();
            if (type == "text") {
                if (!block.contains("text") || !block["text"].is_string()) {
                    continue;
                }
                const std::string value = block["text"].get<std::string>();
                text += value;
                parts.push_back({{"type", "text"}, {"text", value}});
            } else if (type == "image") {
                if (!block.contains("source") || !block["source"].is_object()) {
                    continue;
                }
                const auto& source = block["source"];
                std::string url;
                if (!source.contains("type") || !source["type"].is_string()) {
                    continue;
                }
                if (source["type"] == "base64" &&
                    source.contains("media_type") && source["media_type"].is_string() &&
                    source.contains("data") && source["data"].is_string()) {
                    url = "data:" + source["media_type"].get<std::string>() +
                          ";base64," + source["data"].get<std::string>();
                } else if (source["type"] == "url" &&
                           source.contains("url") && source["url"].is_string()) {
                    url = source["url"].get<std::string>();
                } else {
                    continue;
                }
                has_images = true;
                parts.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", url}}}
                });
            }
        }
        return has_images ? parts : json(text);
    }

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

static int count_tokenizer_response_tokens(const json& tokenize_response) {
    if (tokenize_response.contains("tokens") && tokenize_response["tokens"].is_array()) {
        return static_cast<int>(tokenize_response["tokens"].size());
    }
    if (tokenize_response.contains("count") && tokenize_response["count"].is_number_integer()) {
        return tokenize_response["count"].get<int>();
    }
    if (tokenize_response.contains("input_tokens") && tokenize_response["input_tokens"].is_number_integer()) {
        return tokenize_response["input_tokens"].get<int>();
    }
    return 0;
}

static bool has_tokenizer_response_count(const json& response) {
    return (response.contains("tokens") && response["tokens"].is_array()) ||
           (response.contains("count") && response["count"].is_number_integer()) ||
           (response.contains("input_tokens") && response["input_tokens"].is_number_integer());
}

}  // namespace

void OllamaApi::register_anthropic_routes(httplib::Server& server, const std::shared_ptr<OllamaApi>& self) {
    auto messages_handler = [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_messages(req, res);
    };
    auto count_tokens_handler = [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_count_tokens(req, res);
    };

    server.Post("/v1/messages", messages_handler);
    server.Post("/v1/messages/count_tokens", count_tokens_handler);
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
            std::vector<std::string> thinking_parts;

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

                        if (source_type == "url" && source.contains("url") && source["url"].is_string()) {
                            std::string url = source["url"].get<std::string>();
                            if (!url.empty()) {
                                has_non_text = true;
                                content_parts.push_back({
                                    {"type", "image_url"},
                                    {"image_url", {{"url", url}}}
                                });
                                continue;
                            }
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

                        std::string tool_id = block.value("id", std::string());
                        if (tool_id.empty()) {
                            tool_id = generate_anthropic_tool_id();
                        }
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

                        json tool_content = block.contains("content")
                            ? convert_anthropic_tool_result_content(block["content"])
                            : json("");

                        if (block.value("is_error", false)) {
                            if (tool_content.is_string()) {
                                tool_content = "Error: " + tool_content.get<std::string>();
                            } else {
                                tool_content.insert(tool_content.begin(), {
                                    {"type", "text"},
                                    {"text", "Error: "}
                                });
                            }
                        }

                        tool_result_messages.push_back({
                            {"role", "tool"},
                            {"tool_call_id", tool_use_id},
                            {"content", tool_content}
                        });
                        continue;
                    }

                    if (type == "thinking") {
                        if (role != "assistant") {
                            add_warning(warnings, "Ignored thinking block outside assistant role");
                            continue;
                        }

                        if (!block.value("signature", std::string()).empty()) {
                            add_warning(warnings, "Ignored unverifiable signature on thinking block");
                        }
                        if (block.contains("thinking") && block["thinking"].is_string()) {
                            thinking_parts.push_back(block["thinking"].get<std::string>());
                        } else {
                            add_warning(warnings, "Ignored thinking block missing string thinking field");
                        }
                        continue;
                    }

                    if (type == "redacted_thinking") {
                        add_warning(warnings, "Ignored redacted_thinking block");
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

            if (!thinking_parts.empty()) {
                openai_msg["reasoning_content"] = join_strings(thinking_parts);
            }

            bool has_content = !content_parts.empty();
            bool has_tool_calls = !assistant_tool_calls.empty();

            for (const auto& tool_msg : tool_result_messages) {
                messages.push_back(tool_msg);
            }

            bool is_tool_result_only = (role == "user" && !has_content && !has_tool_calls && !tool_result_messages.empty());
            if (!is_tool_result_only && (role == "assistant" || has_content || has_tool_calls)) {
                messages.push_back(openai_msg);
            }
        }
    }

    openai_req["messages"] = messages;

    if (anthropic_request.contains("max_tokens")) {
        openai_req["max_tokens"] = anthropic_request["max_tokens"];
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
                json selected_tools = json::array();
                if (openai_req.contains("tools") && openai_req["tools"].is_array()) {
                    for (const auto& tool : openai_req["tools"]) {
                        if (tool["function"].value("name", std::string()) == name) {
                            selected_tools.push_back(tool);
                        }
                    }
                }
                openai_req["tools"] = selected_tools;
                openai_req["tool_choice"] = "required";
            }
        } else {
            add_warning(warnings, "Ignored unsupported tool_choice.type: " + type);
        }
        if (tc.contains("disable_parallel_tool_use")) {
            openai_req["parallel_tool_calls"] = !tc["disable_parallel_tool_use"].get<bool>();
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
            if (anthropic_request["thinking"].contains("budget_tokens")) {
                openai_req["thinking_budget_tokens"] = anthropic_request["thinking"]["budget_tokens"];
            }
        } else if (thinking_type == "adaptive") {
            openai_req["enable_thinking"] = true;
        } else if (thinking_type == "disabled") {
            openai_req["enable_thinking"] = false;
        } else {
            add_warning(warnings, "Ignored unsupported thinking.type: " + thinking_type);
        }
        if (anthropic_request["thinking"].contains("display")) {
            add_warning(warnings, "Local backends do not support thinking.display");
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
                                                 std::vector<std::string>& warnings) {
    std::string response_text;
    std::string thinking_text;
    json tool_blocks = json::array();
    std::string stop_reason = "end_turn";
    json stop_sequence = nullptr;
    std::string response_id = openai_response.value("id", std::string());
    if (response_id.empty()) {
        response_id = generate_anthropic_message_id();
    }

    if (openai_response.contains("choices") && openai_response["choices"].is_array() &&
        !openai_response["choices"].empty()) {
        const auto& choice = openai_response["choices"][0];
        stop_reason = map_finish_reason_to_anthropic_stop_reason(choice, &warnings);
        stop_sequence = extract_stop_sequence(openai_response, choice);
        if (!stop_sequence.is_null()) {
            stop_reason = "stop_sequence";
        }

        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            if (message.contains("content") && message["content"].is_string()) {
                auto split = split_inline_thinking_tags(message["content"].get<std::string>());
                if (split.found_thinking) {
                    append_joined_text(thinking_text, split.thinking);
                    response_text = split.text;
                } else {
                    response_text = message["content"].get<std::string>();
                }
            } else if (message.contains("content") && message["content"].is_array()) {
                std::vector<std::string> text_blocks;
                for (const auto& block : message["content"]) {
                    if (block.is_object() && block.value("type", "") == "text" &&
                        block.contains("text") && block["text"].is_string()) {
                        auto split = split_inline_thinking_tags(block["text"].get<std::string>());
                        if (split.found_thinking) {
                            append_joined_text(thinking_text, split.thinking);
                            if (!split.text.empty()) {
                                text_blocks.push_back(split.text);
                            }
                        } else {
                            text_blocks.push_back(block["text"].get<std::string>());
                        }
                    }
                }
                response_text = join_strings(text_blocks);
            }

            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                append_joined_text(thinking_text, message["reasoning_content"].get<std::string>());
            }

            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& tool_call : message["tool_calls"]) {
                    if (!tool_call.is_object()) {
                        continue;
                    }

                    std::string tool_id = tool_call.value("id", std::string());
                    if (tool_id.empty()) {
                        tool_id = generate_anthropic_tool_id();
                    }
                    std::string tool_name;
                    if (tool_call.contains("function") && tool_call["function"].is_object()) {
                        tool_name = tool_call["function"].value("name", "");
                    }
                    if (tool_name.empty()) {
                        add_warning(warnings, "Encountered tool_call without function name");
                        continue;
                    }

                    tool_blocks.push_back({
                        {"type", "tool_use"},
                        {"id", tool_id},
                        {"name", tool_name},
                        {"input", parse_openai_tool_arguments(tool_call, warnings)}
                    });
                }
            }
        }
    }

    json content_blocks = json::array();
    if (!thinking_text.empty()) {
        content_blocks.push_back(make_anthropic_thinking_block(thinking_text));
    }
    if (!response_text.empty() || (content_blocks.empty() && tool_blocks.empty())) {
        content_blocks.push_back({
            {"type", "text"},
            {"text", response_text}
        });
    }
    for (const auto& block : tool_blocks) {
        content_blocks.push_back(block);
    }

    if (stop_reason != "max_tokens" && stop_reason != "refusal") {
        for (const auto& block : content_blocks) {
            if (block.is_object() && block.value("type", "") == "tool_use") {
                stop_reason = "tool_use";
                break;
            }
        }
    }

    AnthropicUsage usage;
    if (openai_response.contains("usage") && openai_response["usage"].is_object()) {
        usage = convert_openai_usage(openai_response["usage"]);
    }

    json anthropic_res = {
        {"id", response_id},
        {"type", "message"},
        {"role", "assistant"},
        {"model", model},
        {"content", content_blocks},
        {"stop_reason", stop_reason},
        {"stop_sequence", stop_sequence},
        {"usage", anthropic_usage_json(usage, true)}
    };

    return anthropic_res;
}

void OllamaApi::stream_openai_sse_to_anthropic_sse(const std::string& openai_body,
                                                   httplib::DataSink& client_sink,
                                                   const std::string& model,
                                                   int initial_input_tokens,
                                                   StreamFn call_router) {
    httplib::DataSink adapter_sink;
    std::string sse_buffer;

    static const std::string open_thinking_tag = "<think>";
    static const std::string close_thinking_tag = "</think>";

    bool sent_message_start = false;
    bool sent_thinking_content_start = false;
    bool sent_thinking_content_stop = false;
    bool sent_text_content_start = false;
    bool sent_text_content_stop = false;
    std::vector<bool> seen_tool_blocks;
    std::vector<bool> started_tool_blocks;
    std::vector<bool> stopped_tool_blocks;
    std::vector<int> tool_content_indices;
    std::vector<std::string> tool_ids;
    std::vector<std::string> tool_names;
    std::vector<std::string> tool_argument_buffers;
    std::string stop_reason = "end_turn";
    json stop_sequence = nullptr;
    AnthropicUsage stream_usage;
    stream_usage.input_tokens = initial_input_tokens;
    std::string message_id = generate_anthropic_message_id();
    int next_content_index = 0;
    int thinking_content_index = -1;
    int text_content_index = -1;
    std::string inline_content_buffer;
    bool inline_thinking_mode = false;
    bool stream_error = false;

    adapter_sink.is_writable = client_sink.is_writable;

    auto send_message_start = [&]() -> bool {
        if (sent_message_start) {
            return true;
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
                {"usage", anthropic_usage_json(stream_usage, true)}
            }}
        };
        if (!write_sse_event(client_sink, "message_start", message_start)) {
            return false;
        }
        sent_message_start = true;
        return true;
    };

    auto send_error_event = [&](const json& error) -> bool {
        int status_code = get_backend_error_status(error);
        stream_error = true;
        return write_sse_event(client_sink, "error", make_anthropic_error(error, status_code));
    };

    auto close_thinking_block = [&]() -> bool {
        if (!sent_thinking_content_start || sent_thinking_content_stop) {
            return true;
        }

        json signature_delta = {
            {"type", "content_block_delta"},
            {"index", thinking_content_index},
            {"delta", {{"type", "signature_delta"}, {"signature", ""}}}
        };
        if (!write_sse_event(client_sink, "content_block_delta", signature_delta)) {
            return false;
        }

        json content_stop = {
            {"type", "content_block_stop"},
            {"index", thinking_content_index}
        };
        if (!write_sse_event(client_sink, "content_block_stop", content_stop)) {
            return false;
        }
        sent_thinking_content_stop = true;
        return true;
    };

    auto close_text_block = [&]() -> bool {
        if (!sent_text_content_start || sent_text_content_stop) {
            return true;
        }

        json content_stop = {
            {"type", "content_block_stop"},
            {"index", text_content_index}
        };
        if (!write_sse_event(client_sink, "content_block_stop", content_stop)) {
            return false;
        }
        sent_text_content_stop = true;
        return true;
    };

    auto start_thinking_block = [&]() -> bool {
        if (!close_text_block()) {
            return false;
        }
        if (!send_message_start()) {
            return false;
        }
        if (sent_thinking_content_start && !sent_thinking_content_stop) {
            return true;
        }

        thinking_content_index = next_content_index++;
        json content_start = {
            {"type", "content_block_start"},
            {"index", thinking_content_index},
            {"content_block", {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}}
        };
        if (!write_sse_event(client_sink, "content_block_start", content_start)) {
            return false;
        }
        sent_thinking_content_start = true;
        sent_thinking_content_stop = false;
        return true;
    };

    auto send_thinking_delta = [&](const std::string& delta_text) -> bool {
        if (delta_text.empty()) {
            return true;
        }
        if (!start_thinking_block()) {
            return false;
        }

        json content_delta = {
            {"type", "content_block_delta"},
            {"index", thinking_content_index},
            {"delta", {{"type", "thinking_delta"}, {"thinking", delta_text}}}
        };
        return write_sse_event(client_sink, "content_block_delta", content_delta);
    };

    auto start_text_block = [&]() -> bool {
        if (!close_thinking_block()) {
            return false;
        }
        if (!send_message_start()) {
            return false;
        }
        if (sent_text_content_start && !sent_text_content_stop) {
            return true;
        }

        text_content_index = next_content_index++;
        json content_start = {
            {"type", "content_block_start"},
            {"index", text_content_index},
            {"content_block", {{"type", "text"}, {"text", ""}}}
        };
        if (!write_sse_event(client_sink, "content_block_start", content_start)) {
            return false;
        }
        sent_text_content_start = true;
        sent_text_content_stop = false;
        return true;
    };

    auto send_text_delta = [&](const std::string& delta_text) -> bool {
        if (delta_text.empty()) {
            return true;
        }
        if (!start_text_block()) {
            return false;
        }

        json content_delta = {
            {"type", "content_block_delta"},
            {"index", text_content_index},
            {"delta", {{"type", "text_delta"}, {"text", delta_text}}}
        };
        return write_sse_event(client_sink, "content_block_delta", content_delta);
    };

    auto ensure_tool_storage = [&](size_t idx) {
        if (started_tool_blocks.size() <= idx) {
            seen_tool_blocks.resize(idx + 1, false);
            started_tool_blocks.resize(idx + 1, false);
            stopped_tool_blocks.resize(idx + 1, false);
            tool_content_indices.resize(idx + 1, -1);
            tool_ids.resize(idx + 1);
            tool_names.resize(idx + 1);
            tool_argument_buffers.resize(idx + 1);
        }
    };

    auto send_tool_arguments = [&](size_t idx, const std::string& arguments) -> bool {
        if (arguments.empty()) {
            return true;
        }
        json tool_input_delta = {
            {"type", "content_block_delta"},
            {"index", tool_content_indices[idx]},
            {"delta", {
                {"type", "input_json_delta"},
                {"partial_json", arguments}
            }}
        };
        return write_sse_event(client_sink, "content_block_delta", tool_input_delta);
    };

    auto start_tool_block = [&](size_t idx, bool allow_fallback_id) -> bool {
        ensure_tool_storage(idx);
        if (started_tool_blocks[idx]) {
            return true;
        }
        if (tool_names[idx].empty() || (tool_ids[idx].empty() && !allow_fallback_id)) {
            return true;
        }
        if (tool_ids[idx].empty()) {
            tool_ids[idx] = generate_anthropic_tool_id();
        }

        if (!close_thinking_block() || !close_text_block()) {
            return false;
        }
        if (!send_message_start()) {
            return false;
        }

        tool_content_indices[idx] = next_content_index++;
        json tool_block_start = {
            {"type", "content_block_start"},
            {"index", tool_content_indices[idx]},
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
        const std::string buffered_arguments = std::move(tool_argument_buffers[idx]);
        tool_argument_buffers[idx].clear();
        return send_tool_arguments(idx, buffered_arguments);
    };

    auto process_content_delta = [&](const std::string& delta_text) -> bool {
        inline_content_buffer += delta_text;

        while (!inline_content_buffer.empty()) {
            if (inline_thinking_mode) {
                size_t close_pos = inline_content_buffer.find(close_thinking_tag);
                if (close_pos == std::string::npos) {
                    size_t keep = tag_prefix_suffix_length(inline_content_buffer, close_thinking_tag);
                    size_t emit_len = inline_content_buffer.size() - keep;
                    if (emit_len > 0 &&
                        !send_thinking_delta(inline_content_buffer.substr(0, emit_len))) {
                        return false;
                    }
                    inline_content_buffer.erase(0, emit_len);
                    break;
                }

                if (!send_thinking_delta(inline_content_buffer.substr(0, close_pos))) {
                    return false;
                }
                inline_content_buffer.erase(0, close_pos + close_thinking_tag.size());
                inline_thinking_mode = false;
                if (!close_thinking_block()) {
                    return false;
                }
                continue;
            }

            size_t open_pos = inline_content_buffer.find(open_thinking_tag);
            if (open_pos == std::string::npos) {
                size_t keep = tag_prefix_suffix_length(inline_content_buffer, open_thinking_tag);
                size_t emit_len = inline_content_buffer.size() - keep;
                if (emit_len > 0 &&
                    !send_text_delta(inline_content_buffer.substr(0, emit_len))) {
                    return false;
                }
                inline_content_buffer.erase(0, emit_len);
                break;
            }

            if (!send_text_delta(inline_content_buffer.substr(0, open_pos))) {
                return false;
            }
            inline_content_buffer.erase(0, open_pos + open_thinking_tag.size());
            inline_thinking_mode = true;
            if (!start_thinking_block()) {
                return false;
            }
        }

        return true;
    };

    auto flush_inline_content = [&]() -> bool {
        if (inline_content_buffer.empty()) {
            return true;
        }

        std::string pending = inline_content_buffer;
        inline_content_buffer.clear();
        if (inline_thinking_mode) {
            return send_thinking_delta(pending);
        }
        return send_text_delta(pending);
    };

    adapter_sink.write = [&client_sink,
                          &sse_buffer,
                          &sent_message_start,
                          &send_message_start,
                          &send_error_event,
                          &stream_error,
                          &send_thinking_delta,
                          &process_content_delta,
                          &flush_inline_content,
                          &ensure_tool_storage,
                          &start_tool_block,
                          &tool_content_indices,
                          &seen_tool_blocks,
                          &started_tool_blocks,
                          &stopped_tool_blocks,
                          &tool_ids,
                          &tool_names,
                          &tool_argument_buffers,
                          &send_tool_arguments,
                          &stop_reason,
                          &stop_sequence,
                          &stream_usage,
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

            if (line.empty() || line.rfind("data:", 0) != 0) {
                continue;
            }

            std::string json_str = line.substr(5);
            if (!json_str.empty() && json_str.front() == ' ') {
                json_str.erase(0, 1);
            }
            if (json_str == "[DONE]") {
                continue;
            }

            try {
                auto openai_chunk = json::parse(json_str);

                if (openai_chunk.contains("error")) {
                    if (!send_error_event(openai_chunk["error"])) {
                        return false;
                    }
                    continue;
                }

                if (stream_error) {
                    continue;
                }

                if (!sent_message_start) {
                    if (openai_chunk.contains("id") && openai_chunk["id"].is_string()) {
                        const std::string backend_id = openai_chunk["id"].get<std::string>();
                        if (!backend_id.empty()) {
                            message_id = backend_id;
                        }
                    }

                    if (!send_message_start()) {
                        return false;
                    }
                }

                if (openai_chunk.contains("usage") && openai_chunk["usage"].is_object()) {
                    stream_usage = convert_openai_usage(openai_chunk["usage"]);
                }

                if (openai_chunk.contains("choices") && openai_chunk["choices"].is_array() &&
                    !openai_chunk["choices"].empty()) {
                    const auto& choice = openai_chunk["choices"][0];

                    if (choice.contains("delta") && choice["delta"].is_object()) {
                        const auto& delta = choice["delta"];
                        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                            std::string reasoning_delta = delta["reasoning_content"].get<std::string>();
                            if (!reasoning_delta.empty() && !send_thinking_delta(reasoning_delta)) {
                                return false;
                            }
                        }

                        if (delta.contains("content") && delta["content"].is_string()) {
                            std::string delta_text = delta["content"].get<std::string>();
                            if (!delta_text.empty() && !process_content_delta(delta_text)) {
                                return false;
                            }
                        }

                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            if (!flush_inline_content()) {
                                return false;
                            }

                            for (const auto& tool_delta : delta["tool_calls"]) {
                                if (!tool_delta.is_object()) {
                                    continue;
                                }

                                int openai_tool_index = tool_delta.value("index", 0);
                                if (openai_tool_index < 0) {
                                    continue;
                                }

                                size_t idx = static_cast<size_t>(openai_tool_index);
                                ensure_tool_storage(idx);
                                seen_tool_blocks[idx] = true;

                                if (tool_delta.contains("id") && tool_delta["id"].is_string()) {
                                    tool_ids[idx] = tool_delta["id"].get<std::string>();
                                }

                                if (tool_delta.contains("function") && tool_delta["function"].is_object()) {
                                    const auto& fn = tool_delta["function"];
                                    if (fn.contains("name") && fn["name"].is_string()) {
                                        tool_names[idx] = fn["name"].get<std::string>();
                                    }
                                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                        std::string args_delta = fn["arguments"].get<std::string>();
                                        if (!args_delta.empty()) {
                                            if (started_tool_blocks[idx]) {
                                                if (!send_tool_arguments(idx, args_delta)) {
                                                    return false;
                                                }
                                            } else {
                                                tool_argument_buffers[idx] += args_delta;
                                            }
                                        }
                                    }
                                }

                                if (!started_tool_blocks[idx]) {
                                    if (!start_tool_block(idx, false)) {
                                        return false;
                                    }
                                }
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        std::vector<std::string> finish_warnings;
                        stop_reason = map_finish_reason_to_anthropic_stop_reason(choice, &finish_warnings);
                        if (!finish_warnings.empty()) {
                            std::cerr << "[OllamaApi] Anthropic compatibility warnings: "
                                      << warning_header_value(finish_warnings) << std::endl;
                        }
                        json matched_stop = extract_stop_sequence(openai_chunk, choice);
                        if (!matched_stop.is_null()) {
                            stop_sequence = matched_stop;
                            stop_reason = "stop_sequence";
                        }
                    }
                }
            } catch (const std::exception& e) {
                json error = {{"message", std::string("Malformed backend stream chunk: ") + e.what()}};
                if (!send_error_event(error)) {
                    return false;
                }
            }
        }
        return true;
    };

    adapter_sink.done = [&client_sink,
                         &sse_buffer,
                         &send_error_event,
                         &send_message_start,
                         &flush_inline_content,
                         &close_thinking_block,
                         &close_text_block,
                         &start_text_block,
                         &start_tool_block,
                         &sent_message_start,
                         &sent_thinking_content_start,
                         &sent_text_content_start,
                         &stream_error,
                         &seen_tool_blocks,
                         &started_tool_blocks,
                         &stopped_tool_blocks,
                         &tool_content_indices,
                         &tool_names,
                         &stop_reason,
                         &stop_sequence,
                         &stream_usage]() {
        if (!stream_error && !trim_ascii_whitespace(sse_buffer).empty()) {
            json error = {{"message", "Backend stream ended with an incomplete SSE event"}};
            send_error_event(error);
        }
        if (stream_error) {
            client_sink.done();
            return;
        }

        if (!flush_inline_content() || !send_message_start()) {
            client_sink.done();
            return;
        }

        for (size_t idx = 0; idx < started_tool_blocks.size(); ++idx) {
            if (seen_tool_blocks[idx] && !started_tool_blocks[idx]) {
                if (tool_names[idx].empty()) {
                    json error = {{"message", "Backend emitted a tool call without a function name"}};
                    send_error_event(error);
                    client_sink.done();
                    return;
                }
                if (!start_tool_block(idx, true)) {
                    client_sink.done();
                    return;
                }
            }
        }

        bool any_tool_started = false;
        for (bool started : started_tool_blocks) {
            any_tool_started = any_tool_started || started;
        }

        if (!sent_thinking_content_start && !sent_text_content_start && !any_tool_started) {
            if (!start_text_block() || !close_text_block()) {
                client_sink.done();
                return;
            }
        }

        if (!close_thinking_block() || !close_text_block()) {
            client_sink.done();
            return;
        }

        for (size_t idx = 0; idx < started_tool_blocks.size(); ++idx) {
            if (started_tool_blocks[idx] && !stopped_tool_blocks[idx]) {
                json tool_stop = {
                    {"type", "content_block_stop"},
                    {"index", tool_content_indices[idx]}
                };
                if (!write_sse_event(client_sink, "content_block_stop", tool_stop)) {
                    client_sink.done();
                    return;
                }
                stopped_tool_blocks[idx] = true;
            }
        }

        if (stop_reason != "max_tokens" && stop_reason != "refusal") {
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
                {"stop_sequence", stop_sequence}
            }},
            {"usage", anthropic_usage_json(stream_usage, false)}
        };
        if (!write_sse_event(client_sink, "message_delta", message_delta)) {
            client_sink.done();
            return;
        }

        json message_stop = {{"type", "message_stop"}};
        write_sse_event(client_sink, "message_stop", message_stop);
        client_sink.done();
    };

    try {
        call_router(openai_body, adapter_sink);
    } catch (const std::exception& e) {
        if (!stream_error) {
            json error = {{"message", std::string("Backend stream failed: ") + e.what()}};
            send_error_event(error);
        }
        client_sink.done();
    }
}

void OllamaApi::handle_anthropic_count_tokens(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        validate_anthropic_request(request_json, true);
        std::vector<std::string> warnings;

        std::string model = normalize_model_name(request_json.value("model", ""));

        auto openai_req = convert_anthropic_to_openai_chat(request_json, warnings);
        openai_req["stream"] = false;

        try {
            auto_load_model(model);
        } catch (const std::exception&) {
            res.status = 404;
            json error = {{"message", "model '" + model + "' not found, try pulling it first"}};
            res.set_content(make_anthropic_error(error, 404).dump(), "application/json");
            return;
        }

        if (!warnings.empty()) {
            res.set_header("X-Lemonade-Warning", warning_header_value(warnings));
        }

        auto tokenize_response = router_->count_chat_tokens(openai_req);
        if (send_anthropic_backend_error(tokenize_response, res)) {
            return;
        }
        if (!has_tokenizer_response_count(tokenize_response)) {
            res.status = 502;
            json error = {{"message", "Backend returned an invalid token-count response"}};
            res.set_content(make_anthropic_error(error, 502).dump(), "application/json");
            return;
        }

        json response = {{"input_tokens", count_tokenizer_response_tokens(tokenize_response)}};
        res.set_content(response.dump(), "application/json");
    } catch (const json::parse_error& e) {
        res.status = 400;
        json error = {{"message", std::string("Invalid JSON in request body: ") + e.what()}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
    } catch (const json::exception& e) {
        res.status = 400;
        json error = {{"message", std::string("Invalid request body: ") + e.what()}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        json error = {{"message", std::string(e.what())}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in Anthropic count_tokens: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"message", std::string(e.what())}};
        res.set_content(make_anthropic_error(error, 500).dump(), "application/json");
    }
}

void OllamaApi::handle_anthropic_messages(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        validate_anthropic_request(request_json, false);
        std::vector<std::string> warnings;

        std::string model = normalize_model_name(request_json.value("model", ""));

        if (req.has_param("beta")) {
            std::string beta_value = req.get_param_value("beta");
            if (beta_value != "true") {
                add_warning(warnings, "Ignored unsupported beta query value: " + beta_value);
            }
        }

        auto openai_req = convert_anthropic_to_openai_chat(request_json, warnings);

        try {
            auto_load_model(model, extract_auto_load_options(request_json));
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
        int stream_input_tokens = 0;
        if (stream) {
            json count_request = openai_req;
            count_request["stream"] = false;
            try {
                auto count_response = router_->count_chat_tokens(count_request);
                if (has_tokenizer_response_count(count_response)) {
                    stream_input_tokens = count_tokenizer_response_tokens(count_response);
                } else {
                    add_warning(warnings, "Backend could not count input tokens; streaming usage reports input_tokens as 0");
                }
            } catch (const std::exception&) {
                add_warning(warnings, "Backend could not count input tokens; streaming usage reports input_tokens as 0");
            }
            openai_req["stream_options"] = {{"include_usage", true}};
        }
        if (!warnings.empty()) {
            const std::string header = warning_header_value(warnings);
            res.set_header("X-Lemonade-Warning", header);
            std::cerr << "[OllamaApi] Anthropic compatibility warnings: " << header << std::endl;
        }

        if (stream) {
            openai_req["stream"] = true;
            std::string openai_body = openai_req.dump();

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [this, openai_body, model, stream_input_tokens](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;

                    stream_openai_sse_to_anthropic_sse(openai_body, sink, model, stream_input_tokens,
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
        if (send_anthropic_backend_error(openai_response, res)) {
            return;
        }
        if (!openai_response.contains("choices") || !openai_response["choices"].is_array() ||
            openai_response["choices"].empty()) {
            res.status = 502;
            json error = {{"message", "Backend returned an invalid chat-completion response"}};
            res.set_content(make_anthropic_error(error, 502).dump(), "application/json");
            return;
        }
        auto anthropic_response = convert_openai_chat_to_anthropic(openai_response, model, warnings);
        if (!warnings.empty()) {
            const std::string header = warning_header_value(warnings);
            res.set_header("X-Lemonade-Warning", header);
            std::cerr << "[OllamaApi] Anthropic compatibility warnings: " << header << std::endl;
        }
        res.set_content(anthropic_response.dump(), "application/json");

    } catch (const json::parse_error& e) {
        res.status = 400;
        json error = {{"message", std::string("Invalid JSON in request body: ") + e.what()}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
    } catch (const json::exception& e) {
        res.status = 400;
        json error = {{"message", std::string("Invalid request body: ") + e.what()}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
    } catch (const std::invalid_argument& e) {
        res.status = 400;
        json error = {{"message", std::string(e.what())}};
        res.set_content(make_anthropic_error(error, 400).dump(), "application/json");
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
