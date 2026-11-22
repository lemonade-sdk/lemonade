#pragma once

#include "lemon/jobs/job_media_limits.h"
#include "lemon/jobs/job_ops.h"
#include "lemon/jobs/job_types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace lemon {
namespace jobs {

class MediaSinkCapture {
public:
    explicit MediaSinkCapture(const CancelFlag& cancel) : cancel_(cancel) {}

    bool write(const char* data, size_t len) {
        if (!writable()) return false;
        if (len == 0) return true;
        // A byte stream reports failure by writing the error object after the media,
        // so chunk-level detection catches it before it becomes part of the output.
        if (len <= 64u * 1024u) {
            std::string_view chunk(data, len);
            const auto start = chunk.find_first_not_of(" \r\n");
            if (start != std::string_view::npos) {
                chunk.remove_prefix(start);
                if (chunk.front() == '{' || chunk.rfind("data: ", 0) == 0) {
                    if (chunk.front() != '{') chunk.remove_prefix(6);
                    auto payload = nlohmann::json::parse(chunk, nullptr, false);
                    if (payload.is_object() && payload.contains("error")) {
                        stream_error_ = std::move(payload);
                        return false;
                    }
                }
            }
        }
        if (!sse_.has_value()) {
            const std::string_view chunk(data, len);
            sse_ = chunk.rfind("event: ", 0) == 0 || chunk.rfind("data: ", 0) == 0;
        }
        if (len > kMaxJobMediaOutputBytes - buffer_.size()) {
            overflow_ = true;
            return false;
        }
        buffer_.append(data, len);
        if (*sse_) {
            partial_line_.append(data, len);
            size_t start = 0;
            size_t end;
            while ((end = partial_line_.find('\n', start)) != std::string::npos) {
                const std::string line = partial_line_.substr(start, end - start);
                start = end + 1;
                if (line.rfind("data: ", 0) != 0) continue;
                auto payload = nlohmann::json::parse(line.substr(6), nullptr, false);
                if (payload.is_object() && payload.contains("error")) {
                    stream_error_ = std::move(payload);
                    return false;
                }
            }
            partial_line_.erase(0, start);
        }
        return true;
    }

    bool writable() const {
        return !cancel_.load() && !overflow_ && stream_error_.is_null();
    }

    const std::string& buffer() const { return buffer_; }
    bool overflowed() const { return overflow_; }
    const nlohmann::json& stream_error() const { return stream_error_; }

private:
    const CancelFlag& cancel_;
    std::string buffer_;
    bool overflow_ = false;
    std::optional<bool> sse_;
    std::string partial_line_;
    nlohmann::json stream_error_;
};

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
