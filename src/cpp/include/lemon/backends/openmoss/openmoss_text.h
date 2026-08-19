#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace lemon {
namespace backends {
namespace openmoss {
namespace detail {

inline int estimate_max_audio_frames(std::string_view input) {
    std::size_t word_count = 0;
    std::size_t non_whitespace_codepoints = 0;
    bool in_word = false;

    for (const unsigned char byte : input) {
        if ((byte & 0xc0) == 0x80) {
            continue;
        }
        const bool whitespace = byte == ' ' || byte == '\n' || byte == '\r' ||
                                byte == '\t' || byte == '\f' || byte == '\v';
        if (whitespace) {
            in_word = false;
            continue;
        }
        ++non_whitespace_codepoints;
        if (!in_word) {
            ++word_count;
            in_word = true;
        }
    }

    const std::size_t word_units = std::min<std::size_t>(word_count, 200) * 5;
    const std::size_t character_units = std::min<std::size_t>(non_whitespace_codepoints, 1000);
    const std::size_t tokens = std::clamp<std::size_t>(
        std::max(word_units, character_units), 40, 1000);
    return std::max(48, static_cast<int>(tokens * 3 / 2));
}

}  // namespace detail
}  // namespace openmoss
}  // namespace backends
}  // namespace lemon
