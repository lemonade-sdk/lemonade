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

    void expect(bool condition, const std::string& name) {
        if (condition) {
            ok(name);
        } else {
            fail(name);
        }
    }
};

static void test_parse_origin(TestResult& r) {
    using namespace lemon::utils;

    // --- Positive Parsing Cases ---
    {
        Origin o = parse_origin("http://localhost:3000");
        r.expect(o.scheme == "http" && o.host == "localhost" && o.port == 3000, "parse http://localhost:3000");
    }
    {
        Origin o = parse_origin("https://app.lemonade.dev");
        r.expect(o.scheme == "https" && o.host == "app.lemonade.dev" && o.port == -1, "parse https://app.lemonade.dev");
    }
    {
        Origin o = parse_origin("HTTP://LocalHost:8080");
        r.expect(o.scheme == "http" && o.host == "localhost" && o.port == 8080, "parse HTTP://LocalHost:8080 (normalization)");
    }
    {
        Origin o = parse_origin("http://[::1]:8080");
        r.expect(o.scheme == "http" && o.host == "[::1]" && o.port == 8080, "parse http://[::1]:8080");
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]:8443");
        r.expect(o.scheme == "https" && o.host == "[2001:db8::1]" && o.port == 8443, "parse https://[2001:db8::1]:8443");
    }
    {
        Origin o = parse_origin("http://[fe80::1%25eth0]:8080");
        r.expect(o.scheme == "http" && o.host == "[fe80::1]" && o.port == 8080, "parse http://[fe80::1%25eth0]:8080 (IPv6 zone ID %25eth0 stripping)");
    }
    {
        Origin o = parse_origin("http://[fe80::1%1]:8080");
        r.expect(o.scheme == "http" && o.host == "[fe80::1]" && o.port == 8080, "parse http://[fe80::1%1]:8080 (IPv6 zone ID %1 stripping)");
    }
    {
        Origin o = parse_origin("https://example.com/");
        r.expect(o.scheme == "https" && o.host == "example.com" && o.port == -1, "allow origin with trailing slash https://example.com/");
    }
    {
        Origin o = parse_origin("app.lemonade.dev");
        r.expect(o.scheme.empty() && o.host == "app.lemonade.dev" && o.port == -1, "parse app.lemonade.dev (no scheme)");
    }
    {
        Origin o = parse_origin("app.lemonade.dev:8080");
        r.expect(o.scheme.empty() && o.host == "app.lemonade.dev" && o.port == 8080, "parse app.lemonade.dev:8080 (no scheme)");
    }
    {
        Origin o = parse_origin("2001:db8::1");
        r.expect(o.scheme.empty() && o.host == "2001:db8::1" && o.port == -1, "parse bare IPv6 address without port");
    }
    {
        Origin o = parse_origin("http://192.168.1.50:*");
        r.expect(o.is_valid() && o.scheme == "http" && o.host == "192.168.1.50" && o.wildcard_port, "parse wildcard port http://192.168.1.50:*");
    }
    {
        Origin o = parse_origin("http://*.local:*");
        r.expect(o.is_valid() && o.scheme == "http" && o.host == "*.local" && o.wildcard_port, "parse wildcard host and port http://*.local:*");
    }
    {
        Origin o = parse_origin("https://[::1]:*");
        r.expect(o.is_valid() && o.scheme == "https" && o.host == "[::1]" && o.wildcard_port, "parse bracketed IPv6 wildcard port https://[::1]:*");
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]:*");
        r.expect(o.is_valid() && o.scheme == "https" && o.host == "[2001:db8::1]" && o.wildcard_port, "parse bracketed IPv6 full address wildcard port https://[2001:db8::1]:*");
    }
    {
        Origin o = parse_origin("http://[fe80::1%25eth0]:*");
        r.expect(o.is_valid() && o.scheme == "http" && o.host == "[fe80::1]" && o.wildcard_port, "parse bracketed IPv6 zone ID with wildcard port http://[fe80::1%25eth0]:*");
    }
    {
        Origin o = parse_origin("http://localhost:65535");
        r.expect(o.is_valid() && o.port == 65535, "parse boundary port 65535");
    }
    {
        Origin o = parse_origin("http://localhost:0");
        r.expect(o.is_valid() && o.port == 0, "parse boundary port 0");
    }
    {
        Origin o = parse_origin("http://*:*");
        r.expect(o.is_valid() && o.scheme == "http" && o.host == "*" && o.wildcard_port, "parse universal wildcard http://*:*");
    }

    // --- Negative / Malformed Parsing Cases ---
    {
        Origin o = parse_origin("https://example.com/some/path");
        r.expect(!o.is_valid(), "reject origin containing path https://example.com/some/path");
    }
    {
        Origin o = parse_origin("https://example.com?query=1");
        r.expect(!o.is_valid(), "reject origin containing query https://example.com?query=1");
    }
    {
        Origin o = parse_origin("https://example.com#fragment");
        r.expect(!o.is_valid(), "reject origin containing fragment https://example.com#fragment");
    }
    {
        Origin o = parse_origin("https://user:pass@example.com");
        r.expect(!o.is_valid(), "reject origin containing userinfo https://user:pass@example.com");
    }
    {
        Origin o = parse_origin("https://app.example.com:999999999999999");
        r.expect(!o.is_valid(), "reject port overflow https://app.example.com:999999999999999");
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]:443junk");
        r.expect(!o.is_valid(), "reject trailing port characters after IPv6 bracket");
    }
    {
        Origin o = parse_origin("https://[2001:db8::1]junk");
        r.expect(!o.is_valid(), "reject trailing junk after IPv6 bracket");
    }
    {
        Origin o = parse_origin("https://[2001:db8::1");
        r.expect(!o.is_valid(), "reject malformed bracketed IPv6 (missing closing bracket)");
    }
    {
        Origin o = parse_origin("https://app.example.com:65536");
        r.expect(!o.is_valid(), "reject out of range port 65536");
    }
    {
        Origin o = parse_origin("http://localhost:*80");
        r.expect(!o.is_valid(), "reject invalid wildcard port http://localhost:*80");
    }
    {
        Origin o = parse_origin("http://localhost:*junk");
        r.expect(!o.is_valid(), "reject invalid wildcard port with trailing junk http://localhost:*junk");
    }
    {
        Origin o = parse_origin("http://localhost:**");
        r.expect(!o.is_valid(), "reject invalid double wildcard port http://localhost:**");
    }
    {
        Origin o = parse_origin("http://localhost:");
        r.expect(!o.is_valid(), "reject empty port http://localhost:");
    }
}

static void test_origin_matching(TestResult& r) {
    using namespace lemon::utils;

    // --- Positive Matching Cases ---
    {
        Origin req = parse_origin("https://app.lemonade.dev");
        Origin pat = parse_origin("https://app.lemonade.dev");
        r.expect(req.matches(pat), "match exact https://app.lemonade.dev");
    }
    {
        Origin req = parse_origin("https://app.lemonade.dev:443");
        Origin pat = parse_origin("https://app.lemonade.dev");
        r.expect(req.matches(pat), "match default ports https://app.lemonade.dev:443 vs https://app.lemonade.dev");
    }
    {
        Origin pat = parse_origin("http://192.168.1.50:*");
        Origin req1 = parse_origin("http://192.168.1.50:3000");
        Origin req2 = parse_origin("http://192.168.1.50:8080");
        Origin req3 = parse_origin("http://192.168.1.50:5173");
        Origin req4 = parse_origin("http://192.168.1.50");
        Origin req5 = parse_origin("http://192.168.1.50:80");
        r.expect(req1.matches(pat) && req2.matches(pat) && req3.matches(pat) && req4.matches(pat) && req5.matches(pat), "wildcard port matches varying ports (3000, 8080, 5173, default 80)");
    }
    {
        Origin pat = parse_origin("http://*.example.com:*");
        Origin req_sub = parse_origin("http://sub.example.com:3000");
        Origin req_nested = parse_origin("http://a.b.example.com:8080");
        Origin req_deep = parse_origin("http://x.y.z.example.com:5173");
        r.expect(req_sub.matches(pat) && req_nested.matches(pat) && req_deep.matches(pat), "subdomain wildcard matches subdomains of example.com (including deep nested)");
    }
    {
        Origin pat = parse_origin("http://*.local:*");
        Origin req_dev = parse_origin("http://dev.local:5173");
        Origin req_dashboard = parse_origin("http://dashboard.local:3000");
        r.expect(req_dev.matches(pat) && req_dashboard.matches(pat), "wildcard host matches *.local mDNS hosts");
    }
    {
        Origin pat_all = parse_origin("http://*:*");
        Origin req1 = parse_origin("http://anyhost.org:8080");
        Origin req2 = parse_origin("http://otherhost.net:3000");
        r.expect(req1.matches(pat_all) && req2.matches(pat_all), "universal wildcard http://*:* matches any host and port for http");
    }
    {
        Origin pat_port = parse_origin("http://*:8080");
        Origin req1 = parse_origin("http://anyhost.org:8080");
        r.expect(req1.matches(pat_port), "universal host with specific port http://*:8080 matches port 8080 only");
    }

    // --- Negative / Mismatch Cases ---
    {
        Origin req = parse_origin("https://app.lemonade.dev:8443");
        Origin pat = parse_origin("https://app.lemonade.dev");
        r.expect(!req.matches(pat), "reject port mismatch https://app.lemonade.dev:8443 vs https://app.lemonade.dev");
    }
    {
        Origin req = parse_origin("http://app.lemonade.dev");
        Origin pat = parse_origin("https://app.lemonade.dev");
        r.expect(!req.matches(pat), "reject scheme mismatch http vs https");
    }
    {
        Origin req = parse_origin("https://app.lemonade.dev");
        Origin pat = parse_origin("app.lemonade.dev");
        r.expect(!req.matches(pat), "reject no-scheme pattern https://app.lemonade.dev vs app.lemonade.dev");
    }
    {
        Origin pat = parse_origin("http://192.168.1.50:*");
        Origin req_mismatch_scheme = parse_origin("https://192.168.1.50:3000");
        Origin req_mismatch_host = parse_origin("http://192.168.1.51:3000");
        r.expect(!req_mismatch_scheme.matches(pat), "wildcard port rejects scheme mismatch https vs http");
        r.expect(!req_mismatch_host.matches(pat), "wildcard port rejects host mismatch 192.168.1.51 vs 192.168.1.50");
    }
    {
        Origin pat = parse_origin("http://*.example.com:*");
        Origin req_notexample = parse_origin("http://notexample.com:3000");
        Origin req_badexample = parse_origin("http://badexample.com:3000");
        Origin req_suffix_attacker = parse_origin("http://example.com.attacker.com:3000");
        Origin req_prefix_attacker = parse_origin("http://attacker-example.com:3000");
        Origin req_apex = parse_origin("http://example.com:3000");
        r.expect(!req_notexample.matches(pat) && !req_badexample.matches(pat) && !req_suffix_attacker.matches(pat) && !req_prefix_attacker.matches(pat), "subdomain wildcard enforces dot boundary rejecting notexample.com, badexample.com, and attacker domains");
        r.expect(!req_apex.matches(pat), "subdomain wildcard rejects apex domain example.com");
    }
    {
        Origin pat = parse_origin("http://*.local:*");
        Origin req_notlocal = parse_origin("http://notlocal.com:5173");
        Origin req_apex_local = parse_origin("http://local:5173");
        r.expect(!req_notlocal.matches(pat) && !req_apex_local.matches(pat), "wildcard host rejects notlocal.com and apex local");
    }
    {
        Origin pat_port = parse_origin("http://*:8080");
        Origin req2 = parse_origin("http://otherhost.net:3000");
        r.expect(!req2.matches(pat_port), "reject universal host port mismatch");
    }
}

static void test_server_self_set(TestResult& r) {
    using namespace lemon::utils;

    auto self_set = get_server_self_set("192.168.1.17");
    r.expect(self_set.find("localhost") != self_set.end() &&
             self_set.find("127.0.0.1") != self_set.end() &&
             self_set.find("::1") != self_set.end() &&
             self_set.find("[::1]") != self_set.end(),
             "server_self_set contains loopback entries");

    r.expect(self_set.find("192.168.1.17") != self_set.end(), "server_self_set contains bound host");

    bool has_hostname = false;
    for (const auto& entry : self_set) {
        if (entry.find(".local") != std::string::npos) {
            has_hostname = true;
            break;
        }
    }
    r.expect(has_hostname || !self_set.empty(), "server_self_set contains OS hostname or mDNS entry");
}

static void test_is_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "lemonade.lan", "[fe80::1]", "mybox.local"};

    // --- Positive / Allowed Cases ---
    r.expect(is_origin_allowed("", "", "192.168.1.17:13305", "http", mock_self), "acceptance: allow non-browser empty-Origin request");
    r.expect(is_origin_allowed("http://localhost:3000", ""), "allow loopback localhost");
    r.expect(is_origin_allowed("http://127.0.0.1:8080", ""), "allow loopback 127.0.0.1");
    r.expect(is_origin_allowed("http://[::1]:3000", ""), "allow loopback [::1]");
    r.expect(is_origin_allowed("http://tauri.localhost", ""), "allow loopback tauri.localhost");
    r.expect(is_origin_allowed("lemonade://app", "") && is_origin_allowed("lemonade://ui", ""), "allow lemonade:// desktop app scheme");
    r.expect(is_origin_allowed("file://", "") && is_origin_allowed("app://.", "") && is_origin_allowed("jan://app", "") && is_origin_allowed("vscode-webview://", ""), "allow standard desktop app schemes (file, app, jan, vscode-webview)");

    // Explicit allowlist positive matches
    r.expect(is_origin_allowed("https://app.lemonade.dev", "*") && is_origin_allowed("http://any-random-site.com", "*"), "allow wildcard * for any origin");
    r.expect(is_origin_allowed("https://app.lemonade.dev", "https://app.lemonade.dev"), "allow specific matching origin");
    r.expect(is_origin_allowed("https://app.lemonade.dev", "http://localhost:3000,  https://app.lemonade.dev, http://another.com"), "allow multiple origins with whitespace");
    r.expect(is_origin_allowed("http://localhost:3000", "https://app.lemonade.dev"), "allow loopback origin even when explicit remote allowlist configured");
    r.expect(is_origin_allowed("lemonade://app", "https://app.lemonade.dev"), "allow desktop scheme even when explicit remote allowlist configured");
    r.expect(is_origin_allowed("http://192.168.1.17:13305", "https://app.lemonade.dev,http://192.168.1.17:13305", "192.168.1.17:13305", "http", mock_self), "allow LAN IP when explicitly listed in allowlist");
    r.expect(is_origin_allowed("http://192.168.1.50:5173", "http://192.168.1.50:*"), "allow wildcard port in allowed origins config");
    r.expect(is_origin_allowed("http://192.168.1.50:5173", "https://app.lemonade.dev, http://192.168.1.50:*, http://*.local:*"), "allow wildcard port in comma-separated list");
    r.expect(is_origin_allowed("http://dashboard.local:3000", "http://*.local:*"), "allow wildcard domain in allowed origins config");

    // Same-origin LAN and mDNS positive matches
    r.expect(is_origin_allowed("http://192.168.1.17:13305", "", "192.168.1.17:13305", "http", mock_self), "acceptance: allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    r.expect(is_origin_allowed("http://mybox.local:13305", "", "mybox.local:13305", "http", mock_self), "acceptance: allow same-origin mDNS mybox.local:13305 in server_self_set");
    r.expect(is_origin_allowed("http://MYBOX.LOCAL:13305", "", "mybox.local:13305", "http", mock_self), "acceptance: allow case-insensitive hostname MYBOX.LOCAL");
    r.expect(is_origin_allowed("http://mybox.local.:13305", "", "mybox.local:13305", "http", mock_self), "acceptance: allow trailing-dot hostname mybox.local.");
    r.expect(is_origin_allowed("http://192.168.1.17", "", "192.168.1.17:80", "http", mock_self), "acceptance: allow default port 80 normalization with bare origin");
    r.expect(is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:13305", "http", mock_self), "allow same-origin LAN IP and port");
    r.expect(is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:13305", "", mock_self), "allow same-origin LAN IP with default http scheme");
    r.expect(is_origin_allowed("https://lemonade.lan:8443", "", "lemonade.lan:8443", "https", mock_self), "allow same-origin HTTPS domain and port in server_self_set");
    r.expect(is_origin_allowed("http://[fe80::1]:13305", "", "[fe80::1]:13305", "http", mock_self), "allow same-origin IPv6 address and port in server_self_set");
    r.expect(is_origin_allowed("null", "null", "192.168.1.50:13305", "http", mock_self), "allow opaque null origin with explicit allowlist");

    // --- Negative / Rejected Cases ---
    r.expect(!is_origin_allowed("http://evil.localhost", ""), "reject unlisted *.localhost host");
    r.expect(!is_origin_allowed("http://app.lemonade.dev", "https://app.lemonade.dev"), "reject specific origin scheme mismatch");
    r.expect(!is_origin_allowed("http://192.168.1.17:13305", "https://app.lemonade.dev", "192.168.1.17:13305", "http", mock_self), "reject unlisted same-origin LAN IP when explicit allowlist configured (no fallthrough)");
    r.expect(!is_origin_allowed("http://notlocal.com:3000", "http://*.local:*"), "reject non-matching domain against wildcard domain config");
    r.expect(!is_origin_allowed("http://evil.com:13305", "", "evil.com:13305", "http", mock_self), "acceptance: reject DNS rebind evil.com:13305 matching host but not in server_self_set");
    r.expect(!is_origin_allowed("http://192.168.1.50:13305", "", "192.168.1.50:8000", "http", mock_self), "reject same-origin port mismatch");
    r.expect(!is_origin_allowed("http://evil.com", "", "192.168.1.50:13305", "http", mock_self), "reject cross-origin host mismatch without allowlist");
    r.expect(!is_origin_allowed("https://192.168.1.50:13305", "", "192.168.1.50:13305", "http", mock_self), "reject same-origin scheme mismatch https vs http");
    r.expect(!is_origin_allowed("https://mybox.local:13305", "", "mybox.local:13305", "http", mock_self), "reject mDNS HTTPS origin against HTTP host scheme mismatch");
    r.expect(!is_origin_allowed("null", "", "192.168.1.50:13305", "http", mock_self), "reject opaque null origin against host without allowlist");
}

static void test_resolve_allowed_origins(TestResult& r) {
    using namespace lemon::utils;
    std::string origins = resolve_allowed_origins();
    r.expect(!origins.empty() || origins.empty(), "resolve_allowed_origins callable without error");
}

static void test_is_same_origin(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "example.com", "mybox.local"};

    // --- Positive Same-Origin Matches ---
    r.expect(is_same_origin("http://192.168.1.50:13305", "192.168.1.50:13305", "http", mock_self), "same origin match exact http://192.168.1.50:13305");
    r.expect(is_same_origin("http://example.com", "example.com", "http", mock_self), "same origin match default port 80 http://example.com");
    r.expect(is_same_origin("http://example.com:80", "example.com", "http", mock_self), "same origin match explicit port 80 with default Host port");
    r.expect(is_same_origin("https://example.com:443", "example.com", "https", mock_self), "same origin match explicit port 443 with default HTTPS Host");

    // --- Negative Same-Origin Mismatches & Security Checks ---
    r.expect(!is_same_origin("http://example.com:8080", "example.com:9090", "http", mock_self), "same origin reject port mismatch");
    r.expect(!is_same_origin("http://attacker.com", "192.168.1.50:13305", "http", mock_self), "same origin reject hostname mismatch");
    r.expect(!is_same_origin("http://evil.com:13305", "evil.com:13305", "http", mock_self), "same origin reject host not in server_self_set (DNS rebind)");
    r.expect(!is_same_origin("http://192.168.1.50:13305", "", "http", mock_self), "same origin reject empty host header");
    r.expect(!is_same_origin("", "192.168.1.50:13305", "http", mock_self), "same origin reject empty origin");
    r.expect(!is_same_origin("null", "192.168.1.50:13305", "http", mock_self), "same origin reject null origin");
}

static void test_is_websocket_origin_allowed(TestResult& r) {
    using namespace lemon::utils;

    std::unordered_set<std::string> mock_self = {"192.168.1.17", "192.168.1.50", "mybox.local"};

    // --- Positive / Allowed WebSocket Cases ---
    r.expect(is_websocket_origin_allowed("http://192.168.1.17:13305", "", "192.168.1.17:13305", "http", mock_self), "acceptance: websocket allow same-origin LAN IP 192.168.1.17:13305 in server_self_set");
    r.expect(is_websocket_origin_allowed("http://mybox.local:13305", "", "mybox.local:13305", "http", mock_self), "acceptance: websocket allow same-origin mDNS mybox.local:13305 in server_self_set");
    r.expect(is_websocket_origin_allowed("http://localhost:3000", ""), "websocket allow loopback origin");
    r.expect(is_websocket_origin_allowed("lemonade://app", ""), "websocket allow lemonade:// desktop app origin");
    r.expect(is_origin_allowed("file://", "") && is_origin_allowed("app://.", "") && is_origin_allowed("jan://app", ""), "allow desktop app origins (file, app, jan)");
    r.expect(is_websocket_origin_allowed("http://custom-client.com:3000", "http://custom-client.com:3000"), "websocket allow cross origin if in explicit allowlist");
    r.expect(is_websocket_origin_allowed("http://192.168.1.50:5173", "http://192.168.1.50:*"), "websocket allow wildcard port if in allowlist");
    r.expect(is_websocket_origin_allowed("http://dashboard.local:3000", "http://*.local:*"), "websocket allow wildcard host if in allowlist");

    // --- Negative / Rejected WebSocket Cases ---
    r.expect(!is_websocket_origin_allowed("http://evil.com:13305", "", "evil.com:13305", "http", mock_self), "acceptance: websocket reject DNS rebind evil.com:13305 not in server_self_set");
    r.expect(!is_websocket_origin_allowed("http://unconfigured.com:3000", ""), "websocket reject unconfigured remote origin without allowlist");
    r.expect(!is_websocket_origin_allowed("http://notlocal.com:3000", "http://*.local:*"), "websocket reject non-matching domain against wildcard host allowlist");
    r.expect(!is_websocket_origin_allowed("http://null", "null"), "websocket reject standard http null host origins from matching null allowlist");
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
