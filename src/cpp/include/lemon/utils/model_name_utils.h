#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace lemon {
namespace utils {

/**
 * Trim leading and trailing whitespace from a model name or string.
 */
inline std::string trim_string(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

/**
 * Centralized model name normalization helper.
 * Trims leading/trailing whitespace.
 */
inline std::string normalize_model_name(const std::string& name) {
    return trim_string(name);
}

} // namespace utils
} // namespace lemon
