#include "lemon/thinking_controls.h"

namespace lemon {

bool should_disable_thinking(const json& request_json) {
    // enable_thinking takes precedence over thinking when both are present.
    if (request_json.contains("enable_thinking") && request_json["enable_thinking"].is_boolean()) {
        return request_json["enable_thinking"].get<bool>() == false;
    }

    if (request_json.contains("thinking")) {
        const auto& thinking = request_json["thinking"];
        if (thinking.is_boolean()) {
            return thinking.get<bool>() == false;
        }
        if (thinking.is_object()) {
            const std::string type = thinking.value("type", "");
            if (type == "disabled") {
                return true;
            }
            if (type == "enabled") {
                return false;
            }
        }
    }

    return false;
}

bool prepend_no_think_to_last_user_message(json& request_json) {
    if (!request_json.contains("messages") || !request_json["messages"].is_array()) {
        return false;
    }

    auto& messages = request_json["messages"];

    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; i--) {
        if (messages[i].is_object() &&
            messages[i].contains("role") &&
            messages[i]["role"].is_string() &&
            messages[i]["role"].get<std::string>() == "user" &&
            messages[i].contains("content") &&
            messages[i]["content"].is_string()) {

            std::string original_content = messages[i]["content"].get<std::string>();
            messages[i]["content"] = "/no_think\n" + original_content;
            return true;
        }
    }

    return false;
}

bool strip_handled_thinking_fields(json& request_json) {
    bool modified = false;
    modified = request_json.erase("enable_thinking") > 0 || modified;
    modified = request_json.erase("thinking") > 0 || modified;
    return modified;
}

bool normalize_thinking_controls(json& request_json) {
    bool modified = false;
    if (should_disable_thinking(request_json)) {
        modified = prepend_no_think_to_last_user_message(request_json) || modified;
    }
    modified = strip_handled_thinking_fields(request_json) || modified;
    return modified;
}

} // namespace lemon
