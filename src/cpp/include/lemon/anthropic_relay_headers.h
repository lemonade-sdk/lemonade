#pragma once

#include <string>

namespace lemon {
namespace anthropic {

// Header allowlists for the /v1/messages relay to a provider registered with
// --wire-format anthropic. The relay rebuilds the request envelope rather than
// copying it, so anything a client or provider needs to survive the hop has to
// be named here. Both predicates take an already-lowercased name.

// `anthropic-beta` is the consequential one: the SDK and Claude Code send it to
// opt into features such as a 1M-token context window, and dropping it changes
// what upstream accepts. `anthropic-version` is forwarded so a client pinning a
// version wins over the default. Auth, hop-by-hop, Host, and Content-Length are
// excluded because the relay sets its own.
inline bool is_forwardable_request_header(const std::string& lower_name) {
    return lower_name == "anthropic-beta" || lower_name == "anthropic-version";
}

// Without these an SDK cannot honor the provider's rate-limit window, and a bad
// response cannot be escalated to the provider by id.
inline bool is_forwardable_response_header(const std::string& lower_name) {
    return lower_name == "retry-after" || lower_name == "request-id" ||
           lower_name.rfind("anthropic-ratelimit-", 0) == 0;
}

}  // namespace anthropic
}  // namespace lemon
