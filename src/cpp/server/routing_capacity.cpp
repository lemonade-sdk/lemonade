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

// Guard against a pathologically nested body; real shapes are 3 levels deep at
// most (input -> item -> content -> part).
constexpr int kMaxTextDepth = 8;

// UTF-8 byte length of the model-visible text in a field. Handles every shape
// the chat and Responses APIs use:
//   "hi"                                        -> a plain string
//   [{"type":"text","text":"hi"}]               -> chat content parts
//   [{"role":..,"content":[{"type":"input_text","text":"hi"}]}]
//                                               -> Responses input items, whose
//                                                  text sits one level deeper
// Parts carrying no text (image blobs, audio) contribute nothing: their base64
// never reaches the model as prompt text, and counting it would wildly
// over-skip otherwise capable candidates.
int64_t text_bytes(const json& value, int depth = 0) {
    if (value.is_string()) {
        return static_cast<int64_t>(value.get<std::string>().size());
    }
    if (depth >= kMaxTextDepth) {
        return 0;
    }

    int64_t bytes = 0;
    if (value.is_array()) {
        for (const auto& part : value) {
            bytes += text_bytes(part, depth + 1);
        }
        return bytes;
    }
    if (value.is_object()) {
        auto text = value.find("text");
        if (text != value.end() && text->is_string()) {
            bytes += static_cast<int64_t>(text->get<std::string>().size());
        }
        // A Responses input item nests its parts under `content`; recursing
        // reaches them without having to enumerate every part `type`.
        auto content = value.find("content");
        if (content != value.end()) {
            bytes += text_bytes(*content, depth + 1);
        }
    }
    return bytes;
}

int64_t field_text_bytes(const json& request, const char* key) {
    auto it = request.find(key);
    return it == request.end() ? 0 : text_bytes(*it);
}

// Tool definitions and assistant tool calls are injected into the prompt as
// JSON, so their serialized form is the honest cost.
int64_t serialized_bytes(const json& container, const char* key) {
    auto it = container.find(key);
    if (it == container.end() || it->is_null()) {
        return 0;
    }
    return static_cast<int64_t>(it->dump().size());
}

bool text_indicates_overflow(const std::string& message) {
    const std::string lowered = lower_copy(message);
    return lowered.find("exceeds the available context size") != std::string::npos ||
           lowered.find("context length exceeded") != std::string::npos;
}

// One error object is an overflow rejection when its normalized code says so,
// or when its own message text carries a backend rejection phrase. Only these
// two fields are inspected: a message that merely quotes user input must not
// be able to trigger a re-route.
bool error_object_indicates_overflow(const json& error) {
    if (!error.is_object()) {
        return false;
    }
    // `code` is checked for the exact normalized value, and only when it is a
    // string: llama-server sends a numeric `code` (the HTTP status), and
    // json::value() with a string default throws on a type mismatch.
    auto code = error.find("code");
    if (code != error.end() && code->is_string() &&
        code->get<std::string>() == "context_length_exceeded") {
        return true;
    }
    auto message = error.find("message");
    return message != error.end() && message->is_string() &&
           text_indicates_overflow(message->get<std::string>());
}

} // namespace

int64_t estimate_prompt_tokens(const json& request) {
    if (!request.is_object()) {
        return 0;
    }

    int64_t bytes = 0;
    int64_t message_count = 0;

    auto messages = request.find("messages");
    if (messages != request.end() && messages->is_array()) {
        for (const auto& message : *messages) {
            if (!message.is_object()) {
                continue;
            }
            ++message_count;
            bytes += field_text_bytes(message, "content");
            bytes += field_text_bytes(message, "name");
            bytes += serialized_bytes(message, "tool_calls");
        }
    }

    bytes += field_text_bytes(request, "system");
    bytes += field_text_bytes(request, "prompt");
    bytes += serialized_bytes(request, "tools");

    // The Responses API carries the conversation on `input`: either a bare
    // string or an array of message-shaped items. Items pay the same template
    // overhead as chat messages.
    auto input = request.find("input");
    if (input != request.end()) {
        bytes += text_bytes(*input);
        if (input->is_array()) {
            for (const auto& item : *input) {
                if (item.is_object() && item.contains("role")) {
                    ++message_count;
                }
            }
        }
    }

    return (bytes + 3) / 4 + message_count * TOKENS_PER_MESSAGE_OVERHEAD;
}

int64_t generation_headroom(const json& request, int64_t fallback) {
    if (request.is_object()) {
        for (const char* key : {"max_tokens", "max_completion_tokens"}) {
            auto it = request.find(key);
            if (it != request.end() && it->is_number_integer() &&
                it->get<int64_t>() > 0) {
                return it->get<int64_t>();
            }
        }
    }
    return fallback;
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

bool fits(int64_t prompt_tokens, int64_t headroom, int64_t window,
          double safety_margin) {
    if (window <= 0) {
        return true;
    }
    const int64_t required =
        static_cast<int64_t>(std::ceil(safety_margin * static_cast<double>(prompt_tokens))) +
        headroom;
    return required <= window;
}

bool is_context_overflow_error(const json& response) {
    if (!response.is_object()) {
        return false;
    }
    auto error = response.find("error");
    if (error == response.end() || !error->is_object()) {
        return false;
    }
    if (error_object_indicates_overflow(*error)) {
        return true;
    }

    // A cloud provider's rejection is wrapped: the local error carries a
    // generic backend_error code and keeps the provider's own error object
    // under details.response.
    auto details = error->find("details");
    if (details == error->end() || !details->is_object()) {
        return false;
    }
    auto nested = details->find("response");
    if (nested == details->end() || !nested->is_object()) {
        return false;
    }
    auto nested_error = nested->find("error");
    return nested_error != nested->end() &&
           error_object_indicates_overflow(*nested_error);
}

bool sse_event_is_context_overflow(const std::string& event) {
    const std::string marker = "data:";
    const std::size_t pos = event.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    const std::string payload = event.substr(pos + marker.size());
    try {
        return is_context_overflow_error(json::parse(payload));
    } catch (const json::exception&) {
        return false;
    }
}

} // namespace routing_capacity
} // namespace lemon
