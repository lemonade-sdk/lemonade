#pragma once

#include <string>

namespace lemon {
namespace utils {

struct SniffedImage {
    std::string mime;
    std::string extension;
    bool ok() const { return !mime.empty(); }
};

// Identifies an image payload from its magic bytes. Recognizes the formats
// stb_image-based backends can decode; anything else (including WebP) comes
// back empty so callers can reject it up front.
inline SniffedImage sniff_image(const std::string& bytes) {
    auto starts_with = [&bytes](const char* magic, size_t len, size_t offset = 0) {
        return bytes.size() >= offset + len && bytes.compare(offset, len, magic, len) == 0;
    };
    if (starts_with("\x89PNG\r\n\x1a\n", 8)) return {"image/png", "png"};
    if (starts_with("\xFF\xD8\xFF", 3)) return {"image/jpeg", "jpg"};
    if (starts_with("BM", 2)) return {"image/bmp", "bmp"};
    if (starts_with("GIF87a", 6) || starts_with("GIF89a", 6)) return {"image/gif", "gif"};
    return {};
}

}  // namespace utils
}  // namespace lemon
