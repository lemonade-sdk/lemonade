#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

inline std::string select_tts_response_format(
    const nlohmann::json& request,
    const std::vector<std::string>& supported_formats) {
    if (request.contains("stream_format")) {
        return "pcm";
    }
    if (request.contains("response_format") && request["response_format"].is_string()) {
        return request["response_format"].get<std::string>();
    }
    if (!supported_formats.empty()) {
        return supported_formats.front();
    }
    return "mp3";
}

}  // namespace lemon
