#pragma once

#include "lemon/jobs/job_types.h"

#include <map>
#include <string>

namespace lemon {
namespace jobs {

inline std::string data_uri_payload(const std::string& value, std::string* content_type,
                                   bool* is_base64 = nullptr) {
    if (is_base64) *is_base64 = false;
    if (value.rfind("data:", 0) != 0) return value;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return value;
    std::string metadata = value.substr(5, comma - 5);
    const auto base64 = metadata.find(";base64");
    const bool encoded = base64 != std::string::npos &&
        (base64 + 7 == metadata.size() || metadata[base64 + 7] == ';');
    if (is_base64) *is_base64 = encoded;
    if (encoded) metadata.erase(base64, 7);
    if (content_type) *content_type = metadata;
    return value.substr(comma + 1);
}

template <typename Json>
inline void normalize_image_inputs(Json& request, bool backend_uses_image_data) {
    const auto payload = [](const std::string& value, const std::string& field,
                            std::string* content_type = nullptr) {
        bool is_base64 = false;
        auto data = data_uri_payload(value, content_type, &is_base64);
        if (value.rfind("data:", 0) == 0 && !is_base64)
            throw JobError(400, field + " must be base64 or a base64 data URI");
        return data;
    };
    if (!backend_uses_image_data) {
        if (!request.contains("image") && request.contains("image_data") &&
            request["image_data"].is_string()) {
            request["image"] = payload(request["image_data"].template get_ref<const std::string&>(), "image");
        } else if (request.contains("image") && request["image"].is_string()) {
            request["image"] = payload(request["image"].template get_ref<const std::string&>(), "image");
        }
        return;
    }
    for (const std::string key : {"image", "mask"}) {
        const std::string data_key = key + "_data";
        if (!request.contains(data_key) && request.contains(key) && request[key].is_string()) {
            std::string content_type = "image/png";
            request[data_key] = payload(request[key].template get_ref<const std::string&>(), key, &content_type);
            if (key == "image" && !request.contains("image_content_type"))
                request["image_content_type"] = content_type;
            const std::string filename_key = key + "_filename";
            if (!request.contains(filename_key)) request[filename_key] = key + ".png";
        } else if (request.contains(data_key) && request[data_key].is_string()) {
            request[data_key] = payload(request[data_key].template get_ref<const std::string&>(), key);
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
