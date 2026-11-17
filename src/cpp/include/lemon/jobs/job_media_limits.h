#pragma once

#include "lemon/jobs/job_types.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace lemon::jobs {

inline constexpr size_t kMaxJobMediaOutputBytes = 8u * 1024u * 1024u;
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

inline bool job_media_within_budget(size_t stored, size_t added) {
    return stored <= kMaxJobMediaStoreBytes && added <= kMaxJobMediaStoreBytes - stored;
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

inline std::map<std::string, size_t> next_media_extracts(
    const json& output, const json& resolved_extracts,
    const std::map<std::string, size_t>& current, const std::string& step_id) {
    auto next = current;
    // The step output overwrites this context key before its extracts are stored.
    next.erase(step_id);
    const bool media = media_result_bytes(output) != 0;
    for (auto extract = resolved_extracts.begin(); extract != resolved_extracts.end(); ++extract) {
        if (media) next[extract.key()] = payload_bytes(extract.value());
        else next.erase(extract.key());
    }
    return next;
}

inline size_t projected_media_bytes(const Job& job, const json& output,
                                    const std::map<std::string, size_t>& next) {
    size_t total = media_result_bytes(output);
    for (const auto& step : job.steps) {
        const size_t bytes = media_result_bytes(step.output);
        if (bytes > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += bytes;
    }
    for (const auto& extract : next) {
        if (extract.second > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += extract.second;
    }
    return total;
}

inline size_t retained_media_bytes(const Job& job) {
    return projected_media_bytes(job, json::object(), job.media_extract_bytes);
}

} // namespace lemon::jobs
