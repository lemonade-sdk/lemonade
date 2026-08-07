// Unit tests for CloudServer::discovery_policy(), which selects the HTTP trust
// boundary for a provider /v1/models discovery request. The AllowInsecureHttp
// opt-in must only apply to plaintext http:// providers; an https:// provider
// must stay HTTPS-only even when allow_insecure_http is stale or accidentally
// set, since the discovery request carries an Authorization: Bearer header.
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

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
