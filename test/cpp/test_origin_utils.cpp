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

static void test_is_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

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
    if (is_origin_allowed("https://app.lemonade.dev", "*")) {
        r.ok("allow wildcard *");
    } else {
        r.fail("allow wildcard *");
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
    if (!is_origin_allowed("https://app.example.com", "app.example.com")) {
        r.ok("reject host-only config entry (no scheme)");
    } else {
        r.fail("reject host-only config entry (no scheme)");
    }
}

static void test_is_websocket_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

    if (!is_websocket_origin_allowed("http://192.168.1.50:3000", "")) {
        r.ok("websocket reject non-local same origin without explicit allowlist");
    } else {
        r.fail("websocket reject non-local same origin without explicit allowlist");
    }

    if (!is_websocket_origin_allowed("http://192.168.1.50:3000", "")) {
        r.ok("websocket reject cross origin with empty allowlist");
    } else {
        r.fail("websocket reject cross origin with empty allowlist");
    }

    if (is_websocket_origin_allowed("http://192.168.1.50:3000", "http://192.168.1.50:3000")) {
        r.ok("websocket allow cross origin if in allowlist");
    } else {
        r.fail("websocket allow cross origin if in allowlist");
    }

    if (is_websocket_origin_allowed("http://192.168.1.50:3000", "http://192.168.1.50:3000")) {
        r.ok("websocket allow non-local same origin if in allowlist");
    } else {
        r.fail("websocket allow non-local same origin if in allowlist");
    }
}

int main() {
    TestResult r;
    printf("=== OriginUtils Unit Tests ===\n\n");

    test_parse_origin(r);
    test_origin_matching(r);
    test_is_origin_allowed(r);
    test_is_websocket_origin_allowed(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
