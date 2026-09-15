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

    {
        // OpenRouter format: top-level context_length and top_provider completion tokens.
        nlohmann::json openrouter_entry = {
            {"id", "deepseek/deepseek-v4-flash-0731"},
            {"context_length", 1310720},
            {"top_provider", {
                {"context_length", 1048576},
                {"max_completion_tokens", 943718}
            }}
        };
        const auto limits = CloudServer::parse_cloud_limits(openrouter_entry);
        r.check(limits.first == 1310720, "OpenRouter: reads top-level context_length");
        r.check(limits.second == 943718, "OpenRouter: reads top_provider.max_completion_tokens");
    }

    {
        // Fallback to top_provider when top-level context_length is missing.
        nlohmann::json top_provider_fallback = {
            {"id", "provider/model"},
            {"top_provider", {
                {"context_length", 32768},
                {"max_output_tokens", 4096}
            }}
        };
        const auto limits = CloudServer::parse_cloud_limits(top_provider_fallback);
        r.check(limits.first == 32768, "top_provider fallback: reads context_length");
        r.check(limits.second == 4096, "top_provider fallback: reads max_output_tokens");
    }

    {
        // OpenRouter per_request_limits: completion_tokens caps top_provider.max_completion_tokens
        // and per_request_limits.context_length caps context_length.
        nlohmann::json openrouter_capped = {
            {"id", "provider/model"},
            {"context_length", 1310720},
            {"top_provider", {
                {"context_length", 1048576},
                {"max_completion_tokens", 943718}
            }},
            {"per_request_limits", {
                {"context_length", 128000},
                {"completion_tokens", 16384}
            }}
        };
        const auto limits = CloudServer::parse_cloud_limits(openrouter_capped);
        r.check(limits.first == 128000, "per_request_limits.context_length caps context_length");
        r.check(limits.second == 16384, "per_request_limits.completion_tokens caps top_provider output");
    }

    {
        // String numeric values and per_request_limits fallback.
        nlohmann::json string_limits = {
            {"id", "custom/model"},
            {"max_context_length", "65536"},
            {"per_request_limits", {
                {"max_completion_tokens", "8192"}
            }}
        };
        const auto limits = CloudServer::parse_cloud_limits(string_limits);
        r.check(limits.first == 65536, "string conversion: reads max_context_length");
        r.check(limits.second == 8192, "string conversion: reads per_request_limits.max_completion_tokens");
    }

    {
        // Stricter string numeric conversion: trailing junk or negative values are rejected.
        nlohmann::json malformed_limits = {
            {"id", "malformed/model"},
            {"context_length", "128k"},
            {"max_completion_tokens", "-50"}
        };
        const auto limits = CloudServer::parse_cloud_limits(malformed_limits);
        r.check(limits.first == 0, "malformed string with trailing chars rejected -> 0");
        r.check(limits.second == 0, "negative value rejected -> 0");
    }

    {
        // Empty / missing limits default to 0
        nlohmann::json bare = {{"id", "bare/model"}};
        const auto limits = CloudServer::parse_cloud_limits(bare);
        r.check(limits.first == 0 && limits.second == 0, "missing limits -> 0, 0");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
