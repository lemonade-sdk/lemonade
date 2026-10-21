#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon {
namespace backends {
namespace cloud {
namespace alpha {

// Translation between the OpenAI chat shape lemond serves and the
// proprietary agent envelope a vendor endpoint (CommandCode's
// /alpha/generate) speaks. All functions here are pure: no network, no
// registry access, so they are unit-testable in isolation.
//
// Observed contract (verified against the live endpoint):
//   POST {alpha_url} with headers Authorization + x-command-code-version
//   Body: {config: {workingDir, date, environment, structure, isGitRepo,
//           currentBranch, mainBranch, gitStatus, recentCommits},
//          memory: "", params: {model, messages, stream}}
//   params.messages[].role accepts only user|assistant|tool — system content
//   must travel in top-level params.system.
//   The stream must be requested with params.stream=true: the endpoint has no
//   usable non-streaming mode, so non-streaming OpenAI callers are served by
//   buffering the event stream and rendering a complete object.
//   Event lines are newline-delimited JSON objects with a "type" field:
//   start, start-step, reasoning-start, reasoning-delta{text}, reasoning-end,
//   text-start, text-delta{text}, text-end, finish-step{finishReason, usage},
//   finish, provider-metadata. usage uses inputTokens/outputTokens naming.

using json = nlohmann::json;

// Builds the proprietary envelope from an OpenAI chat/completions (or
// completions, via prompt) request body. System/developer messages are
// folded into params.system. temperature/top_p/max_tokens pass through when
// numeric. params.stream is always true — callers buffer when the downstream
// client asked for a non-streaming response.
json build_envelope(const std::string& upstream_model, const json& openai_request);

// True when an OpenAI-compatible error response means "this model id is not
// served behind this endpoint" and the proprietary endpoint is worth trying:
// any 404, or a 400 whose body names a missing/unsupported model. Auth,
// quota, and validation errors never qualify.
bool is_model_not_found(int http_status, const json& response_body);

// Accumulated state for one alpha/generate event stream.
struct StreamState {
    std::string text;
    std::string reasoning;
    int64_t input_tokens = -1;
    int64_t output_tokens = -1;
    std::string finish_reason = "stop";
    bool error = false;
    std::string error_message;
    bool done = false;
};

// Folds one parsed event line into state. Unknown event types are ignored so
// a vendor-side addition cannot break translation.
void feed_event(const json& event, StreamState& state);

// Unique completion id for the OpenAI objects rendered from a fallback
// request ("chatcmpl-alpha-<epoch>-<counter>").
std::string new_completion_id();

// Renders accumulated state as a complete OpenAI chat.completion object.
json to_chat_completion(const StreamState& state, const std::string& model,
                        const std::string& completion_id, int64_t created);

// Renders accumulated state as a complete OpenAI legacy completion object
// (for callers of /v1/completions).
json to_text_completion(const StreamState& state, const std::string& model,
                        const std::string& completion_id, int64_t created);

// OpenAI SSE chunk builders for the streaming relay path. include_usage
// mirrors the inbound stream_options contract: usage travels only when the
// downstream client asked for it (the relay caller decides).
std::string sse_role_chunk(const std::string& completion_id, int64_t created,
                           const std::string& model, bool legacy);
std::string sse_content_chunk(const std::string& completion_id, int64_t created,
                              const std::string& model, const std::string& text,
                              bool legacy);
std::string sse_reasoning_chunk(const std::string& completion_id, int64_t created,
                                const std::string& model, const std::string& text);
std::string sse_final_chunk(const StreamState& state, const std::string& completion_id,
                            int64_t created, const std::string& model, bool legacy,
                            bool include_usage);

}  // namespace alpha
}  // namespace cloud
}  // namespace backends
}  // namespace lemon
