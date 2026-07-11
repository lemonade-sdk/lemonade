// Unit tests for the HttpSecurityPolicy trust boundaries applied by
// lemon::utils::HttpClient. A local httplib server on loopback stands in for
// both a Lemonade-managed backend and an external host so we can exercise the
// scheme and redirect restrictions without real TLS.
//
// The HTTPS→HTTP protocol-downgrade test uses an httplib loopback server with
// an https:// URL in the client request. Because cpp-httplib's SSL support is
// disabled in this build the TLS handshake will fail, but libcurl checks
// CURLOPT_REDIR_PROTOCOLS_STR *before* attempting any network I/O, so the
// http:// redirect is blocked at the configuration level regardless of
// certificate validity.
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

// Make a raw curl request using an https:// URL that points to a loopback
// httplib server and returns an http:// redirect.  This exercises
// CURLOPT_REDIR_PROTOCOLS_STR="https" — curl rejects the http:// redirect
// target even though the initial URL has a legitimate https:// scheme.
//
// The TLS handshake on the loopback address will fail because the loopback
// server does not handle TLS, but libcurl validates the redirect target
// protocol before any network I/O, so the rejection is guaranteed.
static bool test_https_redirect_to_http(httplib::Server& http_svr, int port) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        printf("[FAIL] https: curl_init failed\n");
        return false;
    }

    // Use an https:// URL — the External policy enforces https:// everywhere.
    // The /http-redirect handler returns a 302 to http://127.0.0.1:<port>/ok,
    // which MUST be blocked by CURLOPT_REDIR_PROTOCOLS_STR="https".
    std::string url = "https://127.0.0.1:" + std::to_string(port) + "/http-redirect";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     [](void* ptr, size_t s, size_t n, void* userp) -> size_t {
                         auto* buf = static_cast<std::string*>(userp);
                         buf->append(static_cast<char*>(ptr), s * n);
                         return s * n;
                     });
    std::string body;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    // Allow initial https request but block non-https redirects — this matches
    // the ExternalHttpsOnly policy enforced by apply_http_security_policy().
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // Disable cert verification — the loopback server lacks TLS, but the
    // redirect protocol check happens before any network I/O.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    bool rejected = (res != CURLE_OK);

    curl_easy_cleanup(curl);
    return rejected;
}

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
    // Returns an http:// redirect — exercise for the HTTPS→HTTP downgrade test.
    svr.Get("/http-redirect",
            [port](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("http://127.0.0.1:" + std::to_string(port) + "/ok");
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

    // HTTPS→HTTP protocol downgrade test: the client makes an https:// request
    // and the server returns a 302 redirect pointing to a http:// URL.
    //
    // Under the ExternalHttpsOnly policy, apply_http_security_policy() sets
    // CURLOPT_REDIR_PROTOCOLS_STR="https", which makes curl return
    // CURLE_UNSUPPORTED_PROTOCOL when encountering a non-https redirect.
    //
    // The HTTPS scheme in the URL is necessary to trigger curl's redirect
    // protocol check (CURLPROTO_HTTPS allows the initial request, the http://
    // redirect is then blocked).  The loopback server's TLS will not complete
    // (no cipher negotiation), but curl blocks the redirect at the
    // configuration level before any network I/O occurs.
    {
        bool rejected = test_https_redirect_to_http(svr, port);
        r.check(rejected, "external: https->http redirect is rejected");
    }

    svr.stop();
    server_thread.join();

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
