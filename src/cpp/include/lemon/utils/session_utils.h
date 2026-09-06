#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>

namespace lemon {
namespace session {

// Session/client identity extracted from an inbound request. Both telemetry
// (trace grouping) and the cloud relay (upstream continuity) read from this
// single source, so the well-known header allowlist stays centralized and no
// caller passes arbitrary inbound headers upstream.
struct SessionContext {
    std::string session_id;      // trimmed value, empty if none
    std::string session_header;  // matching header name (lowercased), empty if none
    std::string client_id;       // trimmed value, empty if none
    std::string client_header;   // matching header name (lowercased), empty if none
};

// Transport-scoped, populated per request by the HTTP layer. Lives here rather
// than in telemetry so backends can relay it without depending on the
// observability subsystem. Inline thread_local (C++17) needs no separate TU.
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

inline std::string trim_ws(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

inline std::string to_lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// First non-empty match wins: configured headers (in order) take precedence
// over the well-known allowlist. Returns {value, matched-header-lowercased}.
inline std::pair<std::string, std::string> resolve_first(
    const httplib::Request& req,
    const std::vector<std::string>& configured,
    const std::vector<std::string>& well_known) {
    for (const auto& hdr : configured) {
        std::string cleaned = trim_ws(hdr);
        if (!cleaned.empty() && req.has_header(cleaned)) {
            std::string val = trim_ws(req.get_header_value(cleaned));
            if (!val.empty()) return {val, to_lower(cleaned)};
        }
    }
    for (const auto& hdr : well_known) {
        if (req.has_header(hdr)) {
            std::string val = trim_ws(req.get_header_value(hdr));
            if (!val.empty()) return {val, hdr};
        }
    }
    return {"", ""};
}

}  // namespace detail

// Resolve session/client identity from request headers. Configured headers are
// tried first (in order), then the well-known allowlist. The client identity is
// only reported alongside a session, matching the namespacing contract.
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

}  // namespace session
}  // namespace lemon
