#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/routing_policy.h"

// Capacity-aware candidate filtering for router-collection dispatch (#2959).
// Pure helpers: the dispatch loop in server.cpp estimates the request's token
// footprint, resolves each candidate's effective context window, and skips
// candidates that cannot fit. Everything here is deliberately side-effect-free
// so it can be unit tested without a live Router.
namespace lemon {
namespace routing_capacity {

using json = nlohmann::json;

// Per-message cost of the chat template's role markers and separators
// (e.g. `<|im_start|>user\n` ... `<|im_end|>\n`), which the content text
// itself does not account for.
constexpr int64_t TOKENS_PER_MESSAGE_OVERHEAD = 4;

// Rough prompt footprint in tokens. Message *content* text is counted (not the
// serialized JSON envelope, whose keys and quotes are not sent to the model),
// plus a per-message template overhead. `tools` is counted from its serialized
// form because tool schemas really are injected into the prompt as JSON.
int64_t estimate_prompt_tokens(const json& request);

// Requested max_tokens / max_completion_tokens when present and positive, else
// `fallback` (the policy's generation_headroom).
int64_t generation_headroom(const json& request, int64_t fallback);

// Effective context window for a candidate, in tokens. 0 means unknown —
// callers must treat unknown as unconstrained and never skip on it.
//  - loaded_ctx_size: the live resolved runtime window (wins when present)
//  - cloud recipes: the provider-reported max_context_window (may be 0)
//  - pinned_ctx_size: an explicit ctx_size from the model's recipe options,
//    resolved by the caller (kept out of here so this stays link-light)
//  - unloaded llamacpp: the same estimate the load path would produce
//    (compute_auto_context_size)
//  - anything else: registry max_context_window (may be 0)
int64_t effective_context_window(const ModelInfo& info,
                                 std::optional<int64_t> loaded_ctx_size,
                                 std::optional<int64_t> pinned_ctx_size,
                                 double available_memory_gb);

// safety_margin * prompt_tokens + headroom <= window. window <= 0 (unknown)
// always fits.
bool fits(int64_t prompt_tokens, int64_t headroom, int64_t window,
          double safety_margin);

// True when a backend error response is a context-window rejection. The
// normalized code set by create_backend_error_response (wrapped_server.cpp) is
// matched exactly; the raw-text fallback is scoped to the message/code fields
// that actually carry backend rejections — including the provider body cloud
// errors nest under error.details.response — so an error that merely echoes
// user input cannot trigger a re-route.
bool is_context_overflow_error(const json& response);

// True when one SSE event body ("data: {...}") is such a rejection. Used to
// intercept a streaming overflow before any bytes reach the client.
bool sse_event_is_context_overflow(const std::string& event);

} // namespace routing_capacity
} // namespace lemon
