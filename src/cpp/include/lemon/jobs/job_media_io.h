#pragma once

#include "lemon/jobs/job_types.h"

#include <map>
#include <string>

namespace lemon {
namespace jobs {

inline std::string data_uri_payload(const std::string& value, std::string* content_type) {
    if (value.rfind("data:", 0) != 0) return value;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return value;
    if (content_type) {
        *content_type = value.substr(5, comma - 5);
        const auto base64 = content_type->find(";base64");
        if (base64 != std::string::npos) content_type->erase(base64, 7);
    }
    return value.substr(comma + 1);
}

inline void normalize_image_inputs(json& request, bool backend_uses_image_data) {
    if (!backend_uses_image_data) {
        if (!request.contains("image") && request.contains("image_data") &&
            request["image_data"].is_string()) {
            request["image"] = data_uri_payload(request["image_data"].get<std::string>(), nullptr);
        } else if (request.contains("image") && request["image"].is_string()) {
            request["image"] = data_uri_payload(request["image"].get<std::string>(), nullptr);
        }
        return;
    }
    for (const std::string key : {"image", "mask"}) {
        const std::string data_key = key + "_data";
        if (!request.contains(data_key) && request.contains(key) && request[key].is_string()) {
            std::string content_type = "image/png";
            request[data_key] = data_uri_payload(request[key].get<std::string>(), &content_type);
            if (key == "image" && !request.contains("image_content_type"))
                request["image_content_type"] = content_type;
            const std::string filename_key = key + "_filename";
            if (!request.contains(filename_key)) request[filename_key] = key + ".png";
        } else if (request.contains(data_key) && request[data_key].is_string()) {
            request[data_key] = data_uri_payload(request[data_key].get<std::string>(), nullptr);
        }
    }
}

inline json media_result_json(const std::string& data_b64, size_t size,
                              const std::string& mime_type, const std::string& response_format,
                              const std::map<std::string, std::string>& headers) {
    json out = {{"data", data_b64}, {"mime_type", mime_type}, {"size", size},
                {"response_format", response_format}};
    if (!headers.empty()) out["headers"] = headers;
    return out;
}

}
}
