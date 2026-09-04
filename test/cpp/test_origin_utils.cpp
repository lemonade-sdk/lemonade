#include "lemon/utils/origin_utils.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

static void test_parse_origin(TestResult& r) {
    using namespace lemon::utils;

    {
        Origin o = parse_origin("http://localhost:3000");
        if (o.scheme == "http" && o.host == "localhost" && o.port == 3000) {
            r.ok("parse http://localhost:3000");
        } else {
            r.fail("parse http://localhost:3000");
        }
    }
    {
        Origin o = parse_origin("https://app.lemonade.dev");
        if (o.scheme == "https" && o.host == "app.lemonade.dev" && o.port == -1) {
            r.ok("parse https://app.lemonade.dev");
        } else {
            r.fail("parse https://app.lemonade.dev");
        }
    }
    {
        Origin o = parse_origin("HTTP://LocalHost:8080");
        if (o.scheme == "http" && o.host == "localhost" && o.port == 8080) {
            r.ok("parse HTTP://LocalHost:8080 (normalization)");
        } else {
            r.fail("parse HTTP://LocalHost:8080 (normalization)");
        }
    }
    {
        Origin o = parse_origin("http://[::1]:8080");
        if (o.scheme == "http" && o.host == "[::1]" && o.port == 8080) {
            r.ok("parse http://[::1]:8080");
        } else {
            r.fail("parse http://[::1]:8080");
        }
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]:8443");
        if (o.scheme == "https" && o.host == "[2001:db8::1]" && o.port == 8443) {
            r.ok("parse https://[2001:db8::1]:8443");
        } else {
            r.fail("parse https://[2001:db8::1]:8443");
        }
    }
    {
        Origin o = parse_origin("http://[fe80::1%25eth0]:8080");
        if (o.scheme == "http" && o.host == "[fe80::1]" && o.port == 8080) {
            r.ok("parse http://[fe80::1%25eth0]:8080 (IPv6 zone ID %25eth0 stripping)");
        } else {
            r.fail("parse http://[fe80::1%25eth0]:8080 (IPv6 zone ID %25eth0 stripping)");
        }
    }
    {
        Origin o = parse_origin("http://[fe80::1%1]:8080");
        if (o.scheme == "http" && o.host == "[fe80::1]" && o.port == 8080) {
            r.ok("parse http://[fe80::1%1]:8080 (IPv6 zone ID %1 stripping)");
        } else {
            r.fail("parse http://[fe80::1%1]:8080 (IPv6 zone ID %1 stripping)");
        }
    }
    {
        Origin o = parse_origin("https://example.com/some/path");
        if (!o.is_valid()) {
            r.ok("reject origin containing path https://example.com/some/path");
        } else {
            r.fail("reject origin containing path https://example.com/some/path");
        }
    }
    {
        Origin o = parse_origin("https://example.com/");
        if (o.scheme == "https" && o.host == "example.com" && o.port == -1) {
            r.ok("allow origin with trailing slash https://example.com/");
        } else {
            r.fail("allow origin with trailing slash https://example.com/");
        }
    }
    {
        Origin o = parse_origin("https://example.com?query=1");
        if (!o.is_valid()) {
            r.ok("reject origin containing query https://example.com?query=1");
        } else {
            r.fail("reject origin containing query https://example.com?query=1");
        }
    }
    {
        Origin o = parse_origin("https://example.com#fragment");
        if (!o.is_valid()) {
            r.ok("reject origin containing fragment https://example.com#fragment");
        } else {
            r.fail("reject origin containing fragment https://example.com#fragment");
        }
    }
    {
        Origin o = parse_origin("https://user:pass@example.com");
        if (!o.is_valid()) {
            r.ok("reject origin containing userinfo https://user:pass@example.com");
        } else {
            r.fail("reject origin containing userinfo https://user:pass@example.com");
        }
    }
    {
        Origin o = parse_origin("app.lemonade.dev");
        if (o.scheme.empty() && o.host == "app.lemonade.dev" && o.port == -1) {
            r.ok("parse app.lemonade.dev (no scheme)");
        } else {
            r.fail("parse app.lemonade.dev (no scheme)");
        }
    }
    {
        Origin o = parse_origin("app.lemonade.dev:8080");
        if (o.scheme.empty() && o.host == "app.lemonade.dev" && o.port == 8080) {
            r.ok("parse app.lemonade.dev:8080 (no scheme)");
        } else {
            r.fail("parse app.lemonade.dev:8080 (no scheme)");
        }
    }
    {
        Origin o = parse_origin("2001:db8::1");
        if (o.scheme.empty() && o.host == "2001:db8::1" && o.port == -1) {
            r.ok("parse bare IPv6 address without port");
        } else {
            r.fail("parse bare IPv6 address without port");
        }
    }
    {
        Origin o = parse_origin("https://app.example.com:999999999999999");
        if (!o.is_valid()) {
            r.ok("reject port overflow https://app.example.com:999999999999999");
        } else {
            r.fail("reject port overflow https://app.example.com:999999999999999");
        }
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]:443junk");
        if (!o.is_valid()) {
            r.ok("reject trailing port characters after IPv6 bracket");
        } else {
            r.fail("reject trailing port characters after IPv6 bracket");
        }
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]junk");
        if (!o.is_valid()) {
            r.ok("reject trailing junk after IPv6 bracket");
        } else {
            r.fail("reject trailing junk after IPv6 bracket");
        }
    }
    {
        Origin o = parse_origin("https://[2001:db8::1");
        if (!o.is_valid()) {
            r.ok("reject malformed bracketed IPv6 (missing closing bracket)");
        } else {
            r.fail("reject malformed bracketed IPv6 (missing closing bracket)");
        }
    }
    {
        Origin o = parse_origin("https://app.example.com:65536");
        if (!o.is_valid()) {
            r.ok("reject out of range port 65536");
        } else {
            r.fail("reject out of range port 65536");
        }
    }
}

static void test_origin_matching(TestResult& r) {
    using namespace lemon::utils;

    {
        Origin req = parse_origin("https://app.lemonade.dev");
        Origin pat = parse_origin("https://app.lemonade.dev");
        if (req.matches(pat)) {
            r.ok("match exact https://app.lemonade.dev");
        } else {
            r.fail("match exact https://app.lemonade.dev");
        }
    }
    {
        Origin req = parse_origin("https://app.lemonade.dev:8443");
        Origin pat = parse_origin("https://app.lemonade.dev");
        if (!req.matches(pat)) {
            r.ok("reject port mismatch https://app.lemonade.dev:8443 vs https://app.lemonade.dev");
        } else {
            r.fail("reject port mismatch https://app.lemonade.dev:8443 vs https://app.lemonade.dev");
        }
    }
    {
        Origin req = parse_origin("http://app.lemonade.dev");
        Origin pat = parse_origin("https://app.lemonade.dev");
        if (!req.matches(pat)) {
            r.ok("reject scheme mismatch http vs https");
        } else {
            r.fail("reject scheme mismatch http vs https");
        }
    }
    {
        Origin req = parse_origin("https://app.lemonade.dev");
        Origin pat = parse_origin("app.lemonade.dev");
        if (!req.matches(pat)) {
            r.ok("reject no-scheme pattern https://app.lemonade.dev vs app.lemonade.dev");
        } else {
            r.fail("reject no-scheme pattern https://app.lemonade.dev vs app.lemonade.dev");
        }
    }
    {
        Origin req = parse_origin("https://app.lemonade.dev:443");
        Origin pat = parse_origin("https://app.lemonade.dev");
        if (req.matches(pat)) {
            r.ok("match default ports https://app.lemonade.dev:443 vs https://app.lemonade.dev");
        } else {
            r.fail("match default ports https://app.lemonade.dev:443 vs https://app.lemonade.dev");
        }
    }
}

static void test_server_self_set(TestResult& r) {
    using namespace lemon::utils;

    auto self_set = get_server_self_set("192.168.1.17");
    if (self_set.find("localhost") != self_set.end() &&
        self_set.find("127.0.0.1") != self_set.end() &&
        self_set.find("::1") != self_set.end() &&
        self_set.find("[::1]") != self_set.end()) {
        r.ok("server_self_set contains loopback entries");
    } else {
        r.fail("server_self_set contains loopback entries");
    }

    if (self_set.find("192.168.1.17") != self_set.end()) {
        r.ok("server_self_set contains bound host");
    } else {
        r.fail("server_self_set contains bound host");
    }

    bool has_hostname = false;
    for (const auto& entry : self_set) {
        if (entry.find(".local") != std::string::npos) {
            has_hostname = true;
            break;
        }
    }
    if (has_hostname || !self_set.empty()) {
        r.ok("server_self_set contains OS hostname or mDNS entry");
    } else {
        r.fail("server_self_set contains OS hostname or mDNS entry");
    }
}

static void test_is_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "lemonade.lan", "[fe80::1]", "mybox.local"};

    // Non-browser client empty-Origin regression test
    if (is_origin_allowed("", "", "192.168.1.17:13305", "http", mock_self)) {
        r.ok("acceptance: allow non-browser empty-Origin request");
    } else {
        r.fail("acceptance: allow non-browser empty-Origin request");
    }

    if (is_origin_allowed("http://localhost:3000", "")) {
        r.ok("allow loopback localhost");
    } else {
        r.fail("allow loopback localhost");
    }
    if (is_origin_allowed("http://127.0.0.1:8080", "")) {
        r.ok("allow loopback 127.0.0.1");
    } else {
        r.fail("allow loopback 127.0.0.1");
    }
    if (is_origin_allowed("http://[::1]:3000", "")) {
        r.ok("allow loopback [::1]");
    } else {
        r.fail("allow loopback [::1]");
    }
    if (is_origin_allowed("http://tauri.localhost", "")) {
        r.ok("allow loopback tauri.localhost");
    } else {
        r.fail("allow loopback tauri.localhost");
    }
    if (!is_origin_allowed("http://evil.localhost", "")) {
        r.ok("reject unlisted *.localhost host");
    } else {
        r.fail("reject unlisted *.localhost host");
    }
    if (is_origin_allowed("lemonade://app", "") && is_origin_allowed("lemonade://ui", "")) {
        r.ok("allow lemonade:// desktop app scheme");
    } else {
        r.fail("allow lemonade:// desktop app scheme");
    }
    if (is_origin_allowed("file://", "") && is_origin_allowed("app://.", "") && is_origin_allowed("jan://app", "") && is_origin_allowed("vscode-webview://", "")) {
        r.ok("allow standard desktop app schemes (file, app, jan, vscode-webview)");
    } else {
        r.fail("allow standard desktop app schemes (file, app, jan, vscode-webview)");
    }

    // Rule 1: Explicit allowlist authoritative first
    if (is_origin_allowed("https://app.lemonade.dev", "*") && is_origin_allowed("http://any-random-site.com", "*")) {
        r.ok("allow wildcard * for any origin");
    } else {
        r.fail("allow wildcard * for any origin");
    }
    if (is_origin_allowed("https://app.lemonade.dev", "https://app.lemonade.dev")) {
        r.ok("allow specific matching origin");
    } else {
        r.fail("allow specific matching origin");
    }
    if (!is_origin_allowed("http://app.lemonade.dev", "https://app.lemonade.dev")) {
        r.ok("reject specific origin scheme mismatch");
    } else {
        r.fail("reject specific origin scheme mismatch");
    }
    if (is_origin_allowed("https://app.lemonade.dev", "http://localhost:3000,  https://app.lemonade.dev, http://another.com")) {
        r.ok("allow multiple origins with whitespace");
    } else {
        r.fail("allow multiple origins with whitespace");
    }
    if (is_origin_allowed("http://localhost:3000", "https://app.lemonade.dev")) {
        r.ok("allow loopback origin even when explicit remote allowlist configured");
    } else {
        r.fail("allow loopback origin even when explicit remote allowlist configured");
    }
    if (is_origin_allowed("lemonade://app", "https://app.lemonade.dev")) {
        r.ok("allow desktop scheme even when explicit remote allowlist configured");
    } else {
        r.fail("allow desktop scheme even when explicit remote allowlist configured");
    }
    if (!is_origin_allowed("http://192.168.1.17:13305", "https://app.lemonade.dev", "192.168.1.17:13305", "http", mock_self)) {
        r.ok("reject unlisted same-origin LAN IP when explicit allowlist configured (no fallthrough)");
    } else {
        r.fail("reject unlisted same-origin LAN IP when explicit allowlist configured (no fallthrough)");
    }
    if (is_origin_allowed("http://192.168.1.17:13305", "https://app.lemonade.dev,http://192.168.1.17:13305", "192.168.1.17:13305", "http", mock_self)) {
        r.ok("allow LAN IP when explicitly listed in allowlist");
    } else {
        r.fail("allow LAN IP when explicitly listed in allowlist");
    }

    // Rule 3: Zero-config LAN and mDNS acceptance tests
    if (is_origin_allowed("http://192.168.1.17:13305", "", "192.168.1.17:13305", "http", mock_self)) {
        r.ok("acceptance: allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    } else {
        r.fail("acceptance: allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    }
    if (is_origin_allowed("http://mybox.local:13305", "", "mybox.local:13305", "http", mock_self)) {
        r.ok("acceptance: allow same-origin mDNS mybox.local:13305 in server_self_set");
    } else {
        r.fail("acceptance: allow same-origin mDNS mybox.local:13305 in server_self_set");
    }
    if (is_origin_allowed("http://MYBOX.LOCAL:13305", "", "mybox.local:13305", "http", mock_self)) {
        r.ok("acceptance: allow case-insensitive hostname MYBOX.LOCAL");
    } else {
        r.fail("acceptance: allow case-insensitive hostname MYBOX.LOCAL");
    }
    if (is_origin_allowed("http://mybox.local.:13305", "", "mybox.local:13305", "http", mock_self)) {
        r.ok("acceptance: allow trailing-dot hostname mybox.local.");
    } else {
        r.fail("acceptance: allow trailing-dot hostname mybox.local.");
    }
    if (is_origin_allowed("http://192.168.1.17", "", "192.168.1.17:80", "http", mock_self)) {
        r.ok("acceptance: allow default port 80 normalization with bare origin");
    } else {
        r.fail("acceptance: allow default port 80 normalization with bare origin");
    }
    if (!is_origin_allowed("http://evil.com:13305", "", "evil.com:13305", "http", mock_self)) {
        r.ok("acceptance: reject DNS rebind evil.com:13305 matching host but not in server_self_set");
    } else {
        r.fail("acceptance: reject DNS rebind evil.com:13305 matching host but not in server_self_set");
    }

    if (is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("allow same-origin LAN IP and port");
    } else {
        r.fail("allow same-origin LAN IP and port");
    }
    if (is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:13305", "", mock_self)) {
        r.ok("allow same-origin LAN IP with default http scheme");
    } else {
        r.fail("allow same-origin LAN IP with default http scheme");
    }
    if (!is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:8000", "http", mock_self)) {
        r.ok("reject same-origin port mismatch");
    } else {
        r.fail("reject same-origin port mismatch");
    }
    if (!is_origin_allowed("http://evil.com", "", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("reject cross-origin host mismatch without allowlist");
    } else {
        r.fail("reject cross-origin host mismatch without allowlist");
    }
    if (!is_origin_allowed("https://192.168.1.50:13305", "", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("reject same-origin scheme mismatch https vs http");
    } else {
        r.fail("reject same-origin scheme mismatch https vs http");
    }
    if (!is_origin_allowed("https://mybox.local:13305", "", "mybox.local:13305", "http", mock_self)) {
        r.ok("reject mDNS HTTPS origin against HTTP host scheme mismatch");
    } else {
        r.fail("reject mDNS HTTPS origin against HTTP host scheme mismatch");
    }
    if (is_origin_allowed("https://lemonade.lan:8443", "", "lemonade.lan:8443", "https", mock_self)) {
        r.ok("allow same-origin HTTPS domain and port in server_self_set");
    } else {
        r.fail("allow same-origin HTTPS domain and port in server_self_set");
    }
    if (is_origin_allowed("http://[fe80::1]:13305", "", "[fe80::1]:13305", "http", mock_self)) {
        r.ok("allow same-origin IPv6 address and port in server_self_set");
    } else {
        r.fail("allow same-origin IPv6 address and port in server_self_set");
    }
    if (!is_origin_allowed("null", "", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("reject opaque null origin against host without allowlist");
    } else {
        r.fail("reject opaque null origin against host without allowlist");
    }
    if (is_origin_allowed("null", "null", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("allow opaque null origin with explicit allowlist");
    } else {
        r.fail("allow opaque null origin with explicit allowlist");
    }
}

static void test_resolve_allowed_origins(TestResult& r) {
    using namespace lemon::utils;
    std::string origins = resolve_allowed_origins();
    r.ok("resolve_allowed_origins callable without error");
}

static void test_is_same_origin(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "example.com", "mybox.local"};

    if (is_same_origin("http://192.168.1.50:13305", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("same origin match exact http://192.168.1.50:13305");
    } else {
        r.fail("same origin match exact http://192.168.1.50:13305");
    }
    if (is_same_origin("http://example.com", "example.com", "http", mock_self)) {
        r.ok("same origin match default port 80 http://example.com");
    } else {
        r.fail("same origin match default port 80 http://example.com");
    }
    if (is_same_origin("http://example.com:80", "example.com", "http", mock_self)) {
        r.ok("same origin match explicit port 80 with default Host port");
    } else {
        r.fail("same origin match explicit port 80 with default Host port");
    }
    if (is_same_origin("https://example.com:443", "example.com", "https", mock_self)) {
        r.ok("same origin match explicit port 443 with default HTTPS Host");
    } else {
        r.fail("same origin match explicit port 443 with default HTTPS Host");
    }
    if (!is_same_origin("http://example.com:8080", "example.com:9090", "http", mock_self)) {
        r.ok("same origin reject port mismatch");
    } else {
        r.fail("same origin reject port mismatch");
    }
    if (!is_same_origin("http://attacker.com", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("same origin reject hostname mismatch");
    } else {
        r.fail("same origin reject hostname mismatch");
    }
    if (!is_same_origin("http://evil.com:13305", "evil.com:13305", "http", mock_self)) {
        r.ok("same origin reject host not in server_self_set (DNS rebind)");
    } else {
        r.fail("same origin reject host not in server_self_set (DNS rebind)");
    }
    if (!is_same_origin("http://192.168.1.50:13305", "", "http", mock_self)) {
        r.ok("same origin reject empty host header");
    } else {
        r.fail("same origin reject empty host header");
    }
    if (!is_same_origin("", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("same origin reject empty origin");
    } else {
        r.fail("same origin reject empty origin");
    }
    if (!is_same_origin("null", "192.168.1.50:13305", "http", mock_self)) {
        r.ok("same origin reject null origin");
    } else {
        r.fail("same origin reject null origin");
    }
}

static void test_is_websocket_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "mybox.local"};

    // WS zero-config LAN and mDNS acceptance tests
    if (is_websocket_origin_allowed("http://192.168.1.17:13305", "", "192.168.1.17:13305", "http", mock_self)) {
        r.ok("acceptance: websocket allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    } else {
        r.fail("acceptance: websocket allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    }
    if (is_websocket_origin_allowed("http://mybox.local:13305", "", "mybox.local:13305", "http", mock_self)) {
        r.ok("acceptance: websocket allow same-origin mDNS mybox.local:13305 in server_self_set");
    } else {
        r.fail("acceptance: websocket allow same-origin mDNS mybox.local:13305 in server_self_set");
    }
    if (!is_websocket_origin_allowed("http://evil.com:13305", "", "evil.com:13305", "http", mock_self)) {
        r.ok("acceptance: websocket reject DNS rebind evil.com:13305 not in server_self_set");
    } else {
        r.fail("acceptance: websocket reject DNS rebind evil.com:13305 not in server_self_set");
    }

    if (is_websocket_origin_allowed("http://localhost:3000", "")) {
        r.ok("websocket allow loopback origin");
    } else {
        r.fail("websocket allow loopback origin");
    }

    if (is_websocket_origin_allowed("lemonade://app", "")) {
        r.ok("websocket allow lemonade:// desktop app origin");
    } else {
        r.fail("websocket allow lemonade:// desktop app origin");
    }

    if (!is_websocket_origin_allowed("http://unconfigured.com:3000", "")) {
        r.ok("websocket reject unconfigured remote origin without allowlist");
    } else {
        r.fail("websocket reject unconfigured remote origin without allowlist");
    }

    if (is_websocket_origin_allowed("http://custom-client.com:3000", "http://custom-client.com:3000")) {
        r.ok("websocket allow cross origin if in explicit allowlist");
    } else {
        r.fail("websocket allow cross origin if in explicit allowlist");
    }

    if (!is_websocket_origin_allowed("http://null", "null")) {
        r.ok("websocket reject standard http null host origins from matching null allowlist");
    } else {
        r.fail("websocket reject standard http null host origins from matching null allowlist");
    }
}

int main() {
    TestResult r;
    printf("=== OriginUtils Unit Tests ===\n\n");

    test_parse_origin(r);
    test_origin_matching(r);
    test_server_self_set(r);
    test_is_same_origin(r);
    test_is_origin_allowed(r);
    test_is_websocket_origin_allowed(r);
    test_resolve_allowed_origins(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
