#pragma once

#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"

// Capacity-aware candidate filtering for router-collection dispatch (#2959).
// Pure helpers: the dispatch loop in server.cpp estimates the request's token
// footprint, resolves each candidate's effective context window, and skips
// candidates that cannot fit. Everything here is deliberately side-effect-free
// so it can be unit tested without a live Router.
namespace lemon {
namespace routing_capacity {

using json = nlohmann::json;

// chars/4 undercounts for code, non-Latin scripts, and chat-template overhead;
// the margin must err toward skipping a too-small candidate.
constexpr double SAFETY_MARGIN = 1.25;

// Generation headroom assumed when the request doesn't set max_tokens.
constexpr int64_t DEFAULT_GENERATION_HEADROOM = 1024;

// Rough prompt footprint in tokens: UTF-8 bytes of the serialized messages,
// system, tools, prompt, and input fields divided by 4 (rounded up). The JSON
// serialization overhead (keys, quotes) stands in for chat-template overhead.
int64_t estimate_prompt_tokens(const json& request);

// Requested max_tokens / max_completion_tokens when present and positive,
// else DEFAULT_GENERATION_HEADROOM.
int64_t generation_headroom(const json& request);

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

// SAFETY_MARGIN * prompt_tokens + headroom <= window. window <= 0 (unknown)
// always fits.
bool fits(int64_t prompt_tokens, int64_t headroom, int64_t window);

// True when a backend error response is a context-window rejection. Matches
// both the normalized code the llama.cpp path sets
// (create_backend_error_response in wrapped_server.cpp) and the raw provider
// rejection text, which cloud errors keep nested under error.details.response
// with a generic backend_error code.
bool is_context_overflow_error(const json& response);

} // namespace routing_capacity
} // namespace lemon
