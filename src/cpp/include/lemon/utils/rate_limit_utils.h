#pragma once

#include <cstdint>
#include <string>

namespace lemon::utils {

// curl --limit-rate style byte rate ("512", "1.5M", "10G"); ""/"0" = 0, invalid = -1.
// Integer-only (mirrors curl GetSizeParameter): value*mul + frac adjust, suffixes K/M/G/T/P.
int64_t parse_rate_limit_to_bytes(const std::string& raw);

} // namespace lemon::utils
