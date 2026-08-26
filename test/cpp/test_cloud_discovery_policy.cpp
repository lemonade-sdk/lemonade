// Unit tests for the CloudServer statics that address an outbound provider
// request: discovery_policy(), upstream_headers(), and upstream_url().
//
// The invariant worth guarding: the AllowInsecureHttp opt-in must apply only to
// plaintext http:// providers. An https:// provider stays HTTPS-only even when
// allow_insecure_http is stale, since the request carries the API key.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <map>
#include <string>

#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/utils/http_client.h>

using lemon::CloudProviderRegistry;
using lemon::backends::CloudServer;
using lemon::utils::HttpSecurityPolicy;

struct TestResult {
    int passed = 0;
    int failed = 0;

    // Not map::at: a dropped header should fail a check, not terminate the run
    // on an uncaught std::out_of_range.
    static std::string header(const std::map<std::string, std::string>& headers,
                              const std::string& name) {
        auto it = headers.find(name);
        return it == headers.end() ? std::string("<absent>") : it->second;
    }

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
        r.check(TestResult::header(headers, "Authorization") == "Bearer sk-test",
                "openai -> auth header is name + prefix + key");
        r.check(headers.count("anthropic-version") == 0,
                "openai -> no anthropic-version header");
    }

    {
        const auto headers = CloudServer::upstream_headers(
            {"x-api-key", ""}, "sk-test", "anthropic");
        r.check(TestResult::header(headers, "x-api-key") == "sk-test",
                "anthropic -> empty prefix sends the bare key");
        r.check(TestResult::header(headers, "anthropic-version") == CloudServer::kAnthropicVersion,
                "anthropic -> anthropic-version is sent");
        r.check(headers.size() == 2,
                "anthropic -> no headers beyond auth and version");
    }

    {
        // The wire format and the auth header are independent settings: a
        // gateway can front the Anthropic format behind bearer auth.
        const auto headers = CloudServer::upstream_headers(
            {"Authorization", "Bearer "}, "sk-test", "anthropic");
        r.check(TestResult::header(headers, "Authorization") == "Bearer sk-test" &&
                    TestResult::header(headers, "anthropic-version") == CloudServer::kAnthropicVersion,
                "anthropic + bearer auth -> both headers sent");
    }

    {
        // Call sites used to hardcode Authorization/Bearer. A provider that
        // sets neither field must still go out byte-identical to that, or the
        // feature silently breaks every existing provider.
        const auto headers = CloudServer::upstream_headers(
            CloudProviderRegistry::AuthHeader{}, "sk-test", "openai");
        r.check(headers.size() == 1 && TestResult::header(headers, "Authorization") == "Bearer sk-test",
                "default AuthHeader + openai -> exactly Authorization: Bearer <key>");
    }

    {
        // A gateway may serve the OpenAI shape at a base with no version
        // segment, so the local "/v1" cannot be assumed to match the
        // provider's layout.
        r.check(CloudServer::upstream_url("https://gw.example.com/OpenAI",
                                          "/v1/chat/completions") ==
                    "https://gw.example.com/OpenAI/chat/completions",
                "base without /v1 -> local /v1 prefix is not duplicated");

        r.check(CloudServer::upstream_url("https://api.example.com/v1",
                                          "/v1/chat/completions") ==
                    "https://api.example.com/v1/chat/completions",
                "base with /v1 -> provider's own version segment is preserved");

        r.check(CloudServer::upstream_url("https://api.example.com/v1/",
                                          "/v1/completions") ==
                    "https://api.example.com/v1/completions",
                "trailing slash on base -> no doubled separator");

        r.check(CloudServer::upstream_url("https://gw.example.com/OpenAI",
                                          "/models") ==
                    "https://gw.example.com/OpenAI/models",
                "discovery path without /v1 -> joined unchanged");

        r.check(CloudServer::upstream_url("https://api.example.com", "/v1x/foo") ==
                    "https://api.example.com/v1x/foo",
                "/v1 is matched as a whole segment, not a byte prefix");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
