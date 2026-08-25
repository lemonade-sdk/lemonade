#include "lemon/routing_capacity.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "lemon/auto_tune.h"

namespace lemon {
namespace routing_capacity {

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int64_t serialized_size(const json& request, const char* key) {
    auto it = request.find(key);
    if (it == request.end() || it->is_null()) {
        return 0;
    }
    return static_cast<int64_t>(it->dump().size());
}

} // namespace

int64_t estimate_prompt_tokens(const json& request) {
    if (!request.is_object()) {
        return 0;
    }
    int64_t bytes = 0;
    bytes += serialized_size(request, "messages");
    bytes += serialized_size(request, "system");
    bytes += serialized_size(request, "tools");
    bytes += serialized_size(request, "prompt");
    bytes += serialized_size(request, "input");
    return (bytes + 3) / 4;
}

int64_t generation_headroom(const json& request) {
    if (request.is_object()) {
        for (const char* key : {"max_tokens", "max_completion_tokens"}) {
            auto it = request.find(key);
            if (it != request.end() && it->is_number_integer() &&
                it->get<int64_t>() > 0) {
                return it->get<int64_t>();
            }
        }
    }
    return DEFAULT_GENERATION_HEADROOM;
}

int64_t effective_context_window(const ModelInfo& info,
                                 std::optional<int64_t> loaded_ctx_size,
                                 std::optional<int64_t> pinned_ctx_size,
                                 double available_memory_gb) {
    if (loaded_ctx_size.has_value() && *loaded_ctx_size > 0) {
        return *loaded_ctx_size;
    }

    if (info.recipe == "cloud") {
        return info.max_context_window > 0 ? info.max_context_window : 0;
    }

    // A ctx_size pinned in the model's recipe options wins over auto-tune,
    // matching the precedence the load path applies.
    if (pinned_ctx_size.has_value() && *pinned_ctx_size > 0) {
        return *pinned_ctx_size;
    }

    if (info.recipe == "llamacpp") {
        return compute_auto_context_size(info, available_memory_gb);
    }

    return info.max_context_window > 0 ? info.max_context_window : 0;
}

bool fits(int64_t prompt_tokens, int64_t headroom, int64_t window) {
    if (window <= 0) {
        return true;
    }
    const int64_t required =
        static_cast<int64_t>(std::ceil(SAFETY_MARGIN * static_cast<double>(prompt_tokens))) +
        headroom;
    return required <= window;
}

bool is_context_overflow_error(const json& response) {
    if (!response.is_object() || !response.contains("error") ||
        !response["error"].is_object()) {
        return false;
    }
    const json& error = response["error"];
    if (error.value("code", std::string()) == "context_length_exceeded") {
        return true;
    }
    const std::string text = lower_copy(error.dump());
    return text.find("exceeds the available context size") != std::string::npos ||
           text.find("context length exceeded") != std::string::npos ||
           text.find("context_length_exceeded") != std::string::npos;
}

} // namespace routing_capacity
} // namespace lemon
