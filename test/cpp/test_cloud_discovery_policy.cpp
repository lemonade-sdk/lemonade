// Unit tests for the CloudServer statics that decide how an outbound provider
// request is addressed: discovery_policy(), which selects the HTTP trust
// boundary, and upstream_headers(), which builds the header map.
//
// discovery_policy(): the AllowInsecureHttp opt-in must only apply to plaintext
// http:// providers; an https:// provider must stay HTTPS-only even when
// allow_insecure_http is stale or accidentally set, since the discovery request
// carries an Authorization: Bearer header.
//
// upstream_headers(): an anthropic-wire-format provider must be sent
// anthropic-version on every request, discovery included, since a strict
// gateway rejects the request without it.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <string>

#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/utils/http_client.h>

using lemon::backends::CloudServer;
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
    printf("=== CloudServer discovery policy Unit Tests ===\n\n");

    r.check(CloudServer::discovery_policy("https://api.example.com/v1", false) ==
                HttpSecurityPolicy::ExternalHttpsOnly,
            "https + allow_insecure_http=false -> ExternalHttpsOnly");

    r.check(CloudServer::discovery_policy("https://api.example.com/v1", true) ==
                HttpSecurityPolicy::ExternalHttpsOnly,
            "https + allow_insecure_http=true -> ExternalHttpsOnly (flag ignored)");

    r.check(CloudServer::discovery_policy("http://127.0.0.1:1234/v1", true) ==
                HttpSecurityPolicy::AllowInsecureHttp,
            "http + allow_insecure_http=true -> AllowInsecureHttp");

    r.check(CloudServer::discovery_policy("http://127.0.0.1:1234/v1", false) ==
                HttpSecurityPolicy::ExternalHttpsOnly,
            "http + allow_insecure_http=false -> ExternalHttpsOnly");

    {
        const auto headers = CloudServer::upstream_headers(
            {"Authorization", "Bearer "}, "sk-test", "openai");
        r.check(headers.at("Authorization") == "Bearer sk-test",
                "openai -> auth header is name + prefix + key");
        r.check(headers.count("anthropic-version") == 0,
                "openai -> no anthropic-version header");
    }

    {
        const auto headers = CloudServer::upstream_headers(
            {"x-api-key", ""}, "sk-test", "anthropic");
        r.check(headers.at("x-api-key") == "sk-test",
                "anthropic -> empty prefix sends the bare key");
        r.check(headers.at("anthropic-version") == CloudServer::kAnthropicVersion,
                "anthropic -> anthropic-version is sent");
        r.check(headers.size() == 2,
                "anthropic -> no headers beyond auth and version");
    }

    {
        // The wire format and the auth header are independent settings: a
        // gateway can front the Anthropic format behind bearer auth.
        const auto headers = CloudServer::upstream_headers(
            {"Authorization", "Bearer "}, "sk-test", "anthropic");
        r.check(headers.at("Authorization") == "Bearer sk-test" &&
                    headers.at("anthropic-version") == CloudServer::kAnthropicVersion,
                "anthropic + bearer auth -> both headers sent");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
