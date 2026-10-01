#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "lemon/error_types.h"

namespace lemon {
namespace anthropic {

// Lemonade and the streaming proxy report an upstream HTTP status as
// "status_code"; llama.cpp's own error bodies carry a numeric "code" instead.
inline int backend_error_http_status(const nlohmann::json& error) {
    if (!error.is_object()) {
        return 500;
    }

    const auto read_status = [](const nlohmann::json& object) {
        for (const char* key : {"status_code", "code"}) {
            if (object.contains(key) && object[key].is_number_integer()) {
                const int status = object[key].get<int>();
                if (status >= 400 && status <= 599) {
                    return status;
                }
            }
        }
        return 0;
    };

    if (const int status = read_status(error); status != 0) {
        return status;
    }
    if (error.contains("details") && error["details"].is_object()) {
        if (const int status = read_status(error["details"]); status != 0) {
            return status;
        }
    }

    const std::string type = error.value("type", "");
    if (type == ErrorType::INVALID_REQUEST || type == "invalid_request_error" ||
        type == ErrorType::UNSUPPORTED_OPERATION) {
        return 400;
    }
    if (type == ErrorType::MODEL_NOT_LOADED) {
        return 404;
    }

    return 500;
}

inline nlohmann::json build_anthropic_error(const nlohmann::json& error, int status) {
    const char* type = "api_error";
    switch (status) {
        case 400: type = "invalid_request_error"; break;
        case 401: type = "authentication_error"; break;
        case 402: type = "billing_error"; break;
        case 403: type = "permission_error"; break;
        case 404: type = "not_found_error"; break;
        case 409: type = "conflict_error"; break;
        case 413: type = "request_too_large"; break;
        case 429: type = "rate_limit_error"; break;
        case 504: type = "timeout_error"; break;
        case 529: type = "overloaded_error"; break;
        default:
            // Anthropic names no type for the remaining 4xx, and every one of
            // them means the caller can fix the request.
            if (status >= 400 && status < 500) {
                type = "invalid_request_error";
            }
            break;
    }

    const std::string message = error.is_object()
        ? error.value("message", "backend error")
        : error.dump();

    return nlohmann::json{
        {"type", "error"},
        {"error", {
            {"type", type},
            {"message", message},
        }},
    };
}

}  // namespace anthropic
}  // namespace lemon
