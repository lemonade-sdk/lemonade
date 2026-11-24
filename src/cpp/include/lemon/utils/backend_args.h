#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace lemon {
namespace utils {

inline bool is_backend_arg_option(const std::string& key) {
    return key == "merge_args" ||
           (key.size() >= 5 && key.compare(key.size() - 5, 5, "_args") == 0);
}

inline nlohmann::json backend_arg_options(const nlohmann::json& options) {
    nlohmann::json result = nlohmann::json::object();
    if (options.is_object()) {
        for (const auto& [key, value] : options.items()) {
            if (is_backend_arg_option(key)) {
                result[key] = value;
            }
        }
    }
    return result;
}

} // namespace utils
} // namespace lemon
