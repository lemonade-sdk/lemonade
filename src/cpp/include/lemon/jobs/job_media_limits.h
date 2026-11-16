#pragma once

#include "lemon/jobs/job_types.h"

#include <cstddef>
#include <limits>

namespace lemon::jobs {

inline constexpr size_t kMaxJobMediaOutputBytes = 8u * 1024u * 1024u;
// Each result lives in the step output and context, bounding the document at
// roughly twice this budget plus base64 growth. Count step outputs because an
// extract can overwrite the context copy.
inline constexpr size_t kMaxJobMediaStoreBytes = 16u * 1024u * 1024u;

inline size_t base64_decoded_size(size_t encoded_len) {
    return (encoded_len / 4) * 3;
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
                image_bytes += base64_decoded_size(encoded->get_ref<const json::string_t&>().size());
            }
        }
    }
    if (!binary && !image) return 0;
    const auto size = result.find("size");
    if (size != result.end() && size->is_number_integer()) {
        if (size->is_number_unsigned()) return size->get<size_t>();
        if (size->get<int64_t>() >= 0) return size->get<size_t>();
    }
    return binary ? base64_decoded_size(data->get_ref<const json::string_t&>().size()) : image_bytes;
}

// Extract copies persist in the context as bare values, not media objects.
inline size_t payload_bytes(const json& value) {
    const auto count = [](const auto& self, const json& current, size_t depth) -> size_t {
        if (current.is_string()) return current.get_ref<const json::string_t&>().size();
        if (!current.is_array() && !current.is_object()) return 0;
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

inline size_t projected_media_bytes(const json& output, const json& resolved_extracts,
                                    const json& context) {
    size_t total = media_result_bytes(output);
    for (auto it = resolved_extracts.begin(); it != resolved_extracts.end(); ++it) {
        size_t added = payload_bytes(it.value());
        const auto previous = context.find(it.key());
        if (previous != context.end() && media_result_bytes(*previous) == 0) {
            // An extract overwrites its key, so only growth beyond the retained value is new data.
            const size_t replaced = payload_bytes(*previous);
            added = added > replaced ? added - replaced : 0;
        }
        if (added > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += added;
    }
    return total;
}

inline size_t retained_media_bytes(const Job& job) {
    size_t total = 0;
    for (const auto& step : job.steps) {
        const size_t bytes = media_result_bytes(step.output);
        if (bytes > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += bytes;
    }
    for (const auto& value : job.context) {
        // Media-shaped entries are already counted through their step outputs.
        if (media_result_bytes(value) != 0) continue;
        const size_t bytes = payload_bytes(value);
        if (bytes > std::numeric_limits<size_t>::max() - total)
            return std::numeric_limits<size_t>::max();
        total += bytes;
    }
    return total;
}

} // namespace lemon::jobs
