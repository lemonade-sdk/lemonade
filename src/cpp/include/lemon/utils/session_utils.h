#pragma once

#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>

#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace session {

// Session/client identity resolved from an inbound request's headers.
struct SessionContext {
    std::string session_id;
    std::string session_header;  // matched header name, lowercased
    std::string client_id;
    std::string client_header;   // matched header name, lowercased
};

// Per-request forwardable session, consumed by cloud backends. Held here rather
// than in telemetry so a backend needn't depend on the observability subsystem.
inline thread_local SessionContext g_request_session;

inline const std::vector<std::string>& well_known_session_headers() {
    static const std::vector<std::string> headers = {
        "x-opencode-session",
        "x-session-id",
        "x-client-session-id",
        "mcp-session-id",
        "x-conversation-id",
        "session-id"
    };
    return headers;
}

inline const std::vector<std::string>& well_known_client_headers() {
    static const std::vector<std::string> headers = {
        "x-opencode-client",
        "x-client-id",
        "x-client-name"
    };
    return headers;
}

namespace detail {

inline std::string to_lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// First non-empty match wins; configured names take precedence over well-known.
// Returns {value, matched-header-lowercased}.
inline std::pair<std::string, std::string> resolve_first(
    const httplib::Request& req,
    const std::vector<std::string>& configured,
    const std::vector<std::string>& well_known) {
    for (const auto& hdr : configured) {
        std::string cleaned = utils::trim_string(hdr);
        if (!cleaned.empty() && req.has_header(cleaned)) {
            std::string val = utils::trim_string(req.get_header_value(cleaned));
            if (!val.empty()) return {val, to_lower(cleaned)};
        }
    }
    for (const auto& hdr : well_known) {
        if (req.has_header(hdr)) {
            std::string val = utils::trim_string(req.get_header_value(hdr));
            if (!val.empty()) return {val, hdr};
        }
    }
    return {"", ""};
}

}  // namespace detail

// Telemetry resolution: configured headers first, then well-known. Client
// identity is only reported alongside a session (namespacing contract).
inline SessionContext resolve_session_context(
    const httplib::Request& req,
    const std::vector<std::string>& configured_session_headers,
    const std::vector<std::string>& configured_client_headers) {
    SessionContext ctx;
    auto session = detail::resolve_first(req, configured_session_headers,
                                         well_known_session_headers());
    if (session.first.empty()) return ctx;

    auto client = detail::resolve_first(req, configured_client_headers,
                                        well_known_client_headers());
    ctx.session_id = std::move(session.first);
    ctx.session_header = std::move(session.second);
    ctx.client_id = std::move(client.first);
    ctx.client_header = std::move(client.second);
    return ctx;
}

// Relay resolution: well-known session headers ONLY. Configured telemetry
// headers (telemetry.session.headers.id) can carry internal identifiers;
// excluding them here is what keeps them from leaking to external providers.
inline SessionContext resolve_forwardable_session(const httplib::Request& req) {
    SessionContext ctx;
    auto session = detail::resolve_first(req, {}, well_known_session_headers());
    ctx.session_id = std::move(session.first);
    ctx.session_header = std::move(session.second);
    return ctx;
}

// No-op when the request carried no well-known session header.
inline void apply_forwardable_session(std::map<std::string, std::string>& headers) {
    const auto& sess = g_request_session;
    if (!sess.session_header.empty() && !sess.session_id.empty()) {
        headers[sess.session_header] = sess.session_id;
    }
}

}  // namespace session
}  // namespace lemon
