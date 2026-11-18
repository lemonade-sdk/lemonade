#pragma once

#include "lemon/jobs/job_types.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace lemon::jobs {

inline constexpr size_t kMaxJobMediaOutputBytes = 8u * 1024u * 1024u;
// Stored payload bytes across both result copies and media-derived extracts.
inline constexpr size_t kMaxJobMediaStoreBytes = 16u * 1024u * 1024u;

inline size_t base64_decoded_size(std::string_view encoded) {
    size_t bytes = (encoded.size() / 4) * 3;
    if (bytes == 0) return 0;
    if (encoded.back() == '=') {
        --bytes;
        if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') --bytes;
    }
    return bytes;
}

inline bool media_output_within_limit(size_t bytes) {
    return bytes <= kMaxJobMediaOutputBytes;
}

inline size_t media_result_bytes(const json& result) {
    if (!result.is_object()) return 0;
    const auto data = result.find("data");
    if (data == result.end()) return 0;
    const auto mime_type = result.find("mime_type");
    const bool binary = data->is_string() && mime_type != result.end() && mime_type->is_string();
    bool image = false;
    size_t image_bytes = 0;
    if (data->is_array()) {
        for (const auto& item : *data) {
            if (!item.is_object()) continue;
            const auto encoded = item.find("b64_json");
            if (encoded != item.end() && encoded->is_string()) {
                image = true;
                image_bytes += base64_decoded_size(encoded->get_ref<const json::string_t&>());
            }
        }
    }
    if (!binary && !image) return 0;
    const auto size = result.find("size");
    if (size != result.end() && size->is_number_integer()) {
        if (size->is_number_unsigned()) return size->get<size_t>();
        if (size->get<int64_t>() >= 0) return size->get<size_t>();
    }
    return binary ? base64_decoded_size(data->get_ref<const json::string_t&>()) : image_bytes;
}

// Extract copies persist in the context as bare values, not media objects.
inline size_t payload_bytes(const json& value) {
    const auto count = [](const auto& self, const json& current, size_t depth) -> size_t {
        if (current.is_string()) return current.get_ref<const json::string_t&>().size();
        if (!current.is_array() && !current.is_object()) return sizeof(json);
        // Exhaust the budget rather than undercount values beyond the recursion bound.
        if (depth >= 32) return std::numeric_limits<size_t>::max();
        size_t total = 0;
        for (const auto& member : current) {
            const size_t bytes = self(self, member, depth + 1);
            if (bytes > std::numeric_limits<size_t>::max() - total)
                return std::numeric_limits<size_t>::max();
            total += bytes;
        }
        return total;
    };
    return count(count, value, 0);
}

inline size_t media_stored_bytes(const json& result) {
    if (media_result_bytes(result) == 0) return 0;
    const size_t bytes = payload_bytes(result);
    if (bytes > std::numeric_limits<size_t>::max() / 2)
        return std::numeric_limits<size_t>::max();
    return 2 * bytes;
}

inline size_t add_media_bytes(size_t total, size_t bytes) {
    if (bytes > std::numeric_limits<size_t>::max() - total)
        return std::numeric_limits<size_t>::max();
    return total + bytes;
}

struct MediaStoreBudget {
    size_t bytes = 0;
    std::map<std::string, size_t> extracts;

    MediaStoreBudget(const Job& job, const json& output, const std::string& step_id)
        : extracts(job.media_extract_bytes) {
        extracts.erase(step_id);
        bytes = media_stored_bytes(output);
        for (const auto& step : job.steps)
            bytes = add_media_bytes(bytes, media_stored_bytes(step.output));
        for (const auto& extract : extracts)
            bytes = add_media_bytes(bytes, extract.second);
    }

    void resolve_extract(const std::string& key, const json& value, bool media) {
        const auto previous = extracts.find(key);
        if (previous != extracts.end()) {
            // Saturated totals must stay saturated to avoid undercounting overflow.
            if (bytes != std::numeric_limits<size_t>::max()) bytes -= previous->second;
            extracts.erase(previous);
        }
        if (media) {
            const size_t added = payload_bytes(value);
            extracts[key] = added;
            bytes = add_media_bytes(bytes, added);
        }
    }
};

} // namespace lemon::jobs
