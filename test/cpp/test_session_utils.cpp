// Unit tests for session/client identity resolution in session_utils.h:
// well-known fallback order, configured-header precedence, whitespace trimming,
// case-insensitive matching, and — critically — the split between telemetry
// resolution (configured + well-known) and the well-known-only allowlist used
// for outbound cloud forwarding.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <httplib.h>

#include <lemon/utils/session_utils.h>

using lemon::session::apply_forwardable_session;
using lemon::session::resolve_forwardable_session;
using lemon::session::resolve_session_context;
using lemon::session::SessionContext;

namespace {

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

httplib::Request make_request(const std::vector<std::pair<std::string, std::string>>& headers) {
    httplib::Request req;
    for (const auto& [name, value] : headers) {
        req.headers.emplace(name, value);
    }
    return req;
}

}  // namespace

int main() {
    TestResult r;
    printf("=== session_utils Unit Tests ===\n\n");

    // --- Well-known resolution and fallback order ---
    {
        auto req = make_request({{"x-session-id", "sess-1"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id == "sess-1" && ctx.session_header == "x-session-id",
                "well-known: lone x-session-id resolves with its name");
    }
    {
        // x-opencode-session precedes x-session-id in the well-known list.
        auto req = make_request({{"x-session-id", "generic"},
                                 {"x-opencode-session", "opencode"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id == "opencode" && ctx.session_header == "x-opencode-session",
                "well-known: list order wins (x-opencode-session over x-session-id)");
    }
    {
        auto req = make_request({{"authorization", "Bearer x"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id.empty() && ctx.session_header.empty(),
                "well-known: no session header -> empty context");
    }

    // --- Configured-header precedence (telemetry) ---
    {
        auto req = make_request({{"x-corp-session", "corp"},
                                 {"x-opencode-session", "opencode"}});
        auto ctx = resolve_session_context(req, {"x-corp-session"}, {});
        r.check(ctx.session_id == "corp" && ctx.session_header == "x-corp-session",
                "configured header beats well-known for telemetry");
    }
    {
        // A configured header that isn't present falls back to well-known.
        auto req = make_request({{"x-opencode-session", "opencode"}});
        auto ctx = resolve_session_context(req, {"x-absent-session"}, {});
        r.check(ctx.session_id == "opencode" && ctx.session_header == "x-opencode-session",
                "absent configured header -> well-known fallback");
    }

    // --- Whitespace trimming and empty/whitespace-only skipping ---
    {
        auto req = make_request({{"x-opencode-session", "  sess-trim  "}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id == "sess-trim",
                "value whitespace is trimmed");
    }
    {
        // Whitespace-only value is skipped; resolution falls through.
        auto req = make_request({{"x-opencode-session", "   "},
                                 {"x-session-id", "real"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id == "real" && ctx.session_header == "x-session-id",
                "whitespace-only header value is skipped");
    }
    {
        // A configured name with surrounding whitespace is trimmed before lookup.
        auto req = make_request({{"x-corp-session", "corp"}});
        auto ctx = resolve_session_context(req, {"  x-corp-session  "}, {});
        r.check(ctx.session_id == "corp" && ctx.session_header == "x-corp-session",
                "configured header name is trimmed before lookup");
    }

    // --- Case-insensitive matching, lowercased header name out ---
    {
        auto req = make_request({{"X-OpenCode-Session", "mixed"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id == "mixed" && ctx.session_header == "x-opencode-session",
                "mixed-case well-known header matches; name returned lowercased");
    }
    {
        auto req = make_request({{"X-Corp-Session", "corp"}});
        auto ctx = resolve_session_context(req, {"X-Corp-Session"}, {});
        r.check(ctx.session_header == "x-corp-session",
                "configured header name returned lowercased");
    }

    // --- Client identity + namespacing contract ---
    {
        auto req = make_request({{"x-opencode-session", "sess"},
                                 {"x-opencode-client", "vscode"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.client_id == "vscode" && ctx.client_header == "x-opencode-client",
                "client identity resolved alongside a session");
    }
    {
        // No session -> client is not reported (namespacing keys on session).
        auto req = make_request({{"x-opencode-client", "vscode"}});
        auto ctx = resolve_session_context(req, {}, {});
        r.check(ctx.session_id.empty() && ctx.client_id.empty(),
                "client without session -> empty context");
    }

    // --- Telemetry resolution vs. cloud forwarding allowlist ---
    {
        // Configured header wins for telemetry, but the well-known-only relay
        // ignores it and forwards the well-known header instead.
        auto req = make_request({{"x-corp-session", "corp-internal"},
                                 {"x-opencode-session", "opencode"}});
        auto tele = resolve_session_context(req, {"x-corp-session"}, {});
        auto fwd = resolve_forwardable_session(req);
        r.check(tele.session_header == "x-corp-session" &&
                    fwd.session_header == "x-opencode-session" &&
                    fwd.session_id == "opencode",
                "telemetry uses configured header; relay uses well-known only");
    }
    {
        // Only an internal configured header present: telemetry captures it,
        // but nothing is forwardable upstream. This is the leak-prevention case.
        auto req = make_request({{"x-corp-session", "corp-internal"}});
        auto tele = resolve_session_context(req, {"x-corp-session"}, {});
        auto fwd = resolve_forwardable_session(req);
        r.check(tele.session_id == "corp-internal" &&
                    fwd.session_id.empty() && fwd.session_header.empty(),
                "configured-only session is never forwarded upstream");
    }

    // --- apply_forwardable_session ---
    {
        lemon::session::g_request_session = {"sess-abc", "x-opencode-session", "", ""};
        std::map<std::string, std::string> headers = {{"Authorization", "Bearer k"}};
        apply_forwardable_session(headers);
        r.check(headers.count("x-opencode-session") == 1 &&
                    headers["x-opencode-session"] == "sess-abc",
                "apply_forwardable_session attaches the session header verbatim");
        lemon::session::g_request_session = {};
    }
    {
        lemon::session::g_request_session = {};
        std::map<std::string, std::string> headers = {{"Authorization", "Bearer k"}};
        apply_forwardable_session(headers);
        r.check(headers.size() == 1,
                "apply_forwardable_session is a no-op with no session");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
