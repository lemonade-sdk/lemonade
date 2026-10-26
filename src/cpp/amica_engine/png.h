#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amica_engine {

// A PNG file holding an 8-bit RGB image, rows top to bottom.
std::string encode_png(std::size_t width, std::size_t height, const std::vector<std::uint8_t>& rgb);

}  // namespace amica_engine
