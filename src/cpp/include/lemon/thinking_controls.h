// Cross-backend "disable thinking" normalization for OpenAI-style chat
// requests. Lemonade's convention: clients express intent via
// `enable_thinking: false` (or the OpenAI-compat `thinking` field), and the
// server normalizes that into a `/no_think` prefix on the last user message —
// the mechanism every backend understands — then strips the handled fields.
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

// Prepend "/no_think\n" to the last string-content user message. Returns true
// if a message was modified.
bool prepend_no_think_to_last_user_message(json& request_json);

// Remove the client-facing thinking fields once handled (backends don't
// understand them). Returns true if anything was removed.
bool strip_handled_thinking_fields(json& request_json);

// The full normalization applied to every outgoing chat request: inject
// /no_think when disabling was requested, then strip the handled fields.
// Returns true if the request was modified.
bool normalize_thinking_controls(json& request_json);

} // namespace lemon
