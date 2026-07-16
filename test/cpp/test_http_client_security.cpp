// Unit tests for the HttpSecurityPolicy trust boundaries applied by
// lemon::utils::HttpClient. A local httplib server on loopback stands in for
// both a Lemonade-managed backend and an external host so we can exercise the
// scheme and redirect restrictions without real TLS.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>
#include <curl/curl.h>

#include <lemon/utils/http_client.h>

using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;

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

int main() {
    TestResult r;
    printf("=== HttpClient security policy Unit Tests ===\n\n");

    httplib::Server svr;

    // Bind port first so route handlers can reference it.
    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        printf("[FAIL] failed to bind loopback test server\n");
        return 1;
    }

    svr.Get("/ok", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });
    svr.Get("/redirect", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/ok");
    });
    // Redirects to itself forever; a bounded client stops at MAXREDIRS.
    svr.Get("/loop", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/loop");
    });
    svr.Get("/to-file", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("file:///etc/passwd");
    });

    std::thread server_thread([&svr]() { svr.listen_after_bind(); });
    svr.wait_until_ready();

    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // Trusted loopback: plain http GET succeeds.
    try {
        auto resp = HttpClient::get(base + "/ok", {}, 5, HttpSecurityPolicy::TrustedLoopback);
        r.check(resp.status_code == 200 && resp.body == "ok",
                "loopback: http GET returns 200");
    } catch (const std::exception& e) {
        r.check(false, std::string("loopback: http GET returns 200 (threw: ") + e.what() + ")");
    }

    // Trusted loopback: redirects are not followed (302 returned verbatim).
    try {
        auto resp = HttpClient::get(base + "/redirect", {}, 5, HttpSecurityPolicy::TrustedLoopback);
        r.check(resp.status_code == 302 && resp.body != "ok",
                "loopback: redirect is not followed");
    } catch (const std::exception& e) {
        r.check(false, std::string("loopback: redirect is not followed (threw: ") + e.what() + ")");
    }

    // External policy: an http:// initial URL is rejected before any request.
    {
        bool rejected = false;
        try {
            HttpClient::get(base + "/ok", {}, 5, HttpSecurityPolicy::ExternalHttpsOnly);
        } catch (const std::exception&) {
            rejected = true;
        }
        r.check(rejected, "external: initial http:// is rejected");
    }

    // Insecure opt-in: plain http GET succeeds.
    try {
        auto resp = HttpClient::get(base + "/ok", {}, 5, HttpSecurityPolicy::AllowInsecureHttp);
        r.check(resp.status_code == 200 && resp.body == "ok",
                "insecure: http GET returns 200");
    } catch (const std::exception& e) {
        r.check(false, std::string("insecure: http GET returns 200 (threw: ") + e.what() + ")");
    }

    // Insecure opt-in: a single http->http redirect within the limit is followed.
    try {
        auto resp = HttpClient::get(base + "/redirect", {}, 5, HttpSecurityPolicy::AllowInsecureHttp);
        r.check(resp.status_code == 200 && resp.body == "ok",
                "insecure: redirect within limit is followed");
    } catch (const std::exception& e) {
        r.check(false, std::string("insecure: redirect within limit is followed (threw: ") + e.what() + ")");
    }

    // Insecure opt-in: a redirect chain past MAXREDIRS is rejected.
    {
        bool rejected = false;
        try {
            HttpClient::get(base + "/loop", {}, 5, HttpSecurityPolicy::AllowInsecureHttp);
        } catch (const std::exception&) {
            rejected = true;
        }
        r.check(rejected, "insecure: redirect chain past the limit is rejected");
    }

    // Insecure opt-in: a redirect to a non-http(s) scheme is rejected.
    {
        bool rejected = false;
        try {
            HttpClient::get(base + "/to-file", {}, 5, HttpSecurityPolicy::AllowInsecureHttp);
        } catch (const std::exception&) {
            rejected = true;
        }
        r.check(rejected, "insecure: redirect to file:// is rejected");
    }

    // Note: An HTTPS→HTTP protocol-downgrade test (client connects to https://
    // and is redirected to http://) is not exercised here. Running a curl
    // request against a plain httplib::Server with an https:// URL fails at the
    // TLS handshake before curl can reach the redirect-protocol check, so the
    // test would pass on an unrelated failure rather than exercising the actual
    // CURLOPT_REDIR_PROTOCOLS_STR="https" logic. That scenario is validated by
    // the ExternalHttpsOnly policy configuration in apply_http_security_policy()
    // and by the AllowInsecureHttp file://-block, which uses the same libcurl
    // mechanism to restrict redirect targets.

    svr.stop();
    server_thread.join();

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
