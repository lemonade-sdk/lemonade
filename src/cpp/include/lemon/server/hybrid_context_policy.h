#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace lemon::hybrid_context_policy {

// These observations describe a real llama.cpp inference, not the number of
// tokens in Pi's outer transcript.  In particular, the last rendered prompt
// includes the chat template and the tool schema.
struct Observation {
    int64_t prompt_tokens = 0;
    size_t history_bytes = 0;
    size_t history_messages = 0;
};

inline int64_t saturating_add(int64_t lhs, uint64_t rhs) {
    if (rhs >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - lhs)) {
        return std::numeric_limits<int64_t>::max();
    }
    return lhs + static_cast<int64_t>(rhs);
}

// If the previous exact prompt occupancy plus the *maximum* possible new
// tokens is nearing the backend's measured slot size, request a checkpoint.
// A byte-level tokenizer cannot encode more tokens than there are UTF-8 bytes;
// the modest per-message allowance covers chat-template role delimiters.
// This is a conservative preflight, not a claim about cache hit rates.
inline bool needs_checkpoint(int64_t slot_ctx, const Observation& observed,
                             size_t current_bytes, size_t current_messages,
                             int64_t occupied_kv_tokens = 0) {
    if (slot_ctx <= 0 || observed.prompt_tokens <= 0) return false;
    const int64_t threshold = slot_ctx - (slot_ctx * 3) / 10;  // reserve 30% for safe checkpointing
    // The slot's actual n_past also includes generated tokens not necessarily
    // present in the previous user-visible transcript. Use it as an additional
    // pressure signal when we have a valid observation for this role.
    if (occupied_kv_tokens >= threshold) return true;
    uint64_t added = current_bytes > observed.history_bytes
        ? static_cast<uint64_t>(current_bytes - observed.history_bytes) : 0;
    if (current_messages > observed.history_messages) {
        const uint64_t extra = static_cast<uint64_t>(current_messages - observed.history_messages);
        const uint64_t remaining = std::numeric_limits<uint64_t>::max() - added;
        added += extra > remaining / 64 ? remaining : extra * 64;
    }
    return saturating_add(observed.prompt_tokens, added) >= threshold;
}

// A checkpoint is not a user-visible completion.  Never recycle a live role
// when it has outstanding external tool calls or when summary generation was
// cut off.  Reject, rather than truncate, long handoffs.
inline void validate_checkpoint(bool has_pending_tools, bool finished_normally,
                                bool has_tool_calls, const std::string& summary,
                                size_t maximum_bytes) {
    if (has_pending_tools) {
        throw std::runtime_error("Hybrid context checkpoint refused with pending tool results");
    }
    if (!finished_normally || has_tool_calls) {
        throw std::runtime_error("Hybrid context checkpoint did not finish normally");
    }
    bool nonblank = false;
    for (unsigned char c : summary) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            nonblank = true;
            break;
        }
    }
    if (!nonblank || summary.size() > maximum_bytes) {
        throw std::runtime_error("Hybrid context checkpoint is empty or excessively long");
    }
}

} // namespace lemon::hybrid_context_policy
