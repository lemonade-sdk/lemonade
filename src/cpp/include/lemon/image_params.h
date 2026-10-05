#pragma once

#include "lemon/recipe_options.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <string>

namespace lemon {
namespace image_params {

inline int random_seed() {
    return static_cast<int>(std::random_device{}() & 0x7fffffffU);
}

// Concrete seed for an image request: a negative seed means "random" for
// Lemonade, so it is replaced by a generated one. Absent or non-integer
// yields nullopt so the backend keeps its own default.
inline std::optional<int> resolve_seed(const nlohmann::json& request) {
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        const int seed = request["seed"].get<int>();
        return seed >= 0 ? seed : random_seed();
    }
    return std::nullopt;
}

// Output size from `size: "WxH"`, request `width`/`height`, or the effective
// recipe options (descriptor defaults included). "" when nothing resolves.
inline std::string resolve_size(const nlohmann::json& request,
                                const RecipeOptions& options) {
    if (request.contains("size") && request["size"].is_string()) {
        return request["size"].get<std::string>();
    }
    if (request.contains("width") && request.contains("height") &&
        request["width"].is_number_integer() && request["height"].is_number_integer()) {
        return std::to_string(request["width"].get<int>()) + "x"
             + std::to_string(request["height"].get<int>());
    }
    const int w = options.get_int_or("width");
    const int h = options.get_int_or("height");
    if (w <= 0 || h <= 0) return "";
    return std::to_string(w) + "x" + std::to_string(h);
}

// Request-body field readers. Presence + type are checked before the value is
// trusted; anything else falls through to the caller's fallback.
inline int request_int(const nlohmann::json& request, const std::string& key, int fallback) {
    if (request.contains(key) && request[key].is_number_integer()) {
        return request[key].get<int>();
    }
    return fallback;
}

inline std::string request_string(const nlohmann::json& request, const std::string& key,
                                  const std::string& fallback) {
    if (request.contains(key) && request[key].is_string()) {
        return request[key].get<std::string>();
    }
    return fallback;
}

inline bool request_bool(const nlohmann::json& request, const std::string& key, bool fallback) {
    if (request.contains(key) && request[key].is_boolean()) {
        return request[key].get<bool>();
    }
    return fallback;
}

// For params where an explicit 0 is meaningful: nullopt means "not in the
// request", so the caller can distinguish "disable" from "unspecified".
inline std::optional<float> request_number(const nlohmann::json& request, const std::string& key) {
    if (request.contains(key) && request[key].is_number()) {
        return request[key].get<float>();
    }
    return std::nullopt;
}

}  // namespace image_params
}  // namespace lemon
