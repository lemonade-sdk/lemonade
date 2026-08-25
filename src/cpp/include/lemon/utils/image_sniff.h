#pragma once

#include <optional>
#include <string>
#include <tuple>

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

// Extract width and height from a PNG image's raw bytes.
// Returns std::nullopt if the data is not a valid PNG with an IHDR chunk.
inline std::optional<std::tuple<int, int>> get_png_dimensions(const std::string& bytes) {
    // 8-byte signature, then IHDR: 4 length + 4 type + data, with big-endian
    // width at offset 16 and height at offset 20. Minimum: 24 bytes.
    if (bytes.size() < 24) return std::nullopt;
    const char sig[] = "\x89PNG\r\n\x1a\n";
    if (bytes.compare(0, 8, sig, 8) != 0) return std::nullopt;
    if (bytes.compare(12, 4, "IHDR", 4) != 0) return std::nullopt;
    int width  = (static_cast<unsigned char>(bytes[16]) << 24) |
                 (static_cast<unsigned char>(bytes[17]) << 16) |
                 (static_cast<unsigned char>(bytes[18]) <<  8) |
                 static_cast<unsigned char>(bytes[19]);
    int height = (static_cast<unsigned char>(bytes[20]) << 24) |
                 (static_cast<unsigned char>(bytes[21]) << 16) |
                 (static_cast<unsigned char>(bytes[22]) <<  8) |
                 static_cast<unsigned char>(bytes[23]);
    if (width <= 0 || height <= 0) return std::nullopt;
    return std::make_tuple(width, height);
}

}  // namespace utils
}  // namespace lemon
