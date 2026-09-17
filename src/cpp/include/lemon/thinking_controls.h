// Cross-backend "disable thinking" normalization for OpenAI-style chat
// requests. Lemonade's public convention is backend-neutral: clients express
// intent via `enable_thinking: false` (or the OpenAI-compat `thinking` field).
//
// The server translates that intent into native OpenAI-compatible reasoning
// controls before backend dispatch. A `/no_think` prefix is retained as a
// compatibility fallback for backends/templates that do not consume those
// controls yet. Client-facing thinking fields are stripped after translation.
//
// Shared by the HTTP request path (server.cpp) and by internal
// classifier/router invocations (routing_classifier_services.cpp), so a
// constrained classifier call is normalized exactly like a normal request.

#pragma once

#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// True if the request asks for thinking to be disabled, via Lemonade's
// `enable_thinking: false` (takes precedence) or the OpenAI-compat
// `thinking: false` / `thinking: {"type": "disabled"}` forms.
bool should_disable_thinking(const json& request_json);

// Prepend "/no_think\n" to the last string-content user message as a legacy
// compatibility fallback. Returns true if a message was modified.
bool prepend_no_think_to_last_user_message(json& request_json);

// Remove the client-facing thinking fields once handled. Returns true if
// anything was removed.
bool strip_handled_thinking_fields(json& request_json);

// Normalize Lemonade's backend-neutral thinking intent into request controls
// understood by reasoning-capable OpenAI-compatible backends. When disabling,
// this sets `reasoning_effort: "none"` and, when possible,
// `chat_template_kwargs.enable_thinking: false`, while retaining `/no_think`
// as a compatibility fallback. The client-facing fields are then stripped.
// Returns true if the request was modified.
bool normalize_thinking_controls(json& request_json);

} // namespace lemon
