// Unit tests for the CloudProviderRegistry per-provider config fields that
// describe how to reach a gateway. All must reproduce the previously
// hardcoded behavior when omitted, and an explicitly empty auth header prefix
// must stay distinct from "not provided" -- some gateways have no prefix.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include <cstdio>
#include <string>

#include <lemon/cloud_provider_registry.h>

#include <nlohmann/json.hpp>

using lemon::CloudProviderRegistry;
using json = nlohmann::json;

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
    printf("=== CloudProviderRegistry config Unit Tests ===\n\n");

    {
        CloudProviderRegistry registry;
        registry.install("fireworks", "https://api.fireworks.ai/inference/v1");
        const auto header = registry.auth_header_for("fireworks");
        r.check(header.name == "Authorization",
                "install() with default options -> default header name Authorization");
        r.check(header.prefix == "Bearer ",
                "install() with default options -> default header prefix 'Bearer '");
    }

    {
        CloudProviderRegistry::InstallOptions options;
        options.auth_header_name = "X-Api-Key";
        options.auth_header_prefix = "";

        CloudProviderRegistry registry;
        registry.install("acme", "https://gateway.example.com/v1", options);
        const auto header = registry.auth_header_for("acme");
        r.check(header.name == "X-Api-Key",
                "install() with explicit header name -> stored verbatim");
        r.check(header.prefix.empty(),
                "install() with explicit empty prefix -> stored as empty, not defaulted");
    }

    {
        // Round-trip through to_config_array() / load_from_config(), the path
        // used to persist and reload config.json across a server restart.
        CloudProviderRegistry::InstallOptions options;
        options.auth_header_name = "X-Api-Key";
        options.auth_header_prefix = "";

        CloudProviderRegistry registry;
        registry.install("acme", "https://gateway.example.com/v1", options);

        CloudProviderRegistry reloaded;
        reloaded.load_from_config(registry.to_config_array());
        const auto header = reloaded.auth_header_for("acme");
        r.check(header.name == "X-Api-Key",
                "round-trip through config array -> header name preserved");
        r.check(header.prefix.empty(),
                "round-trip through config array -> empty prefix preserved");
    }

    {
        // A default-configured provider must not gain new keys in config.json,
        // so upgrading lemonade leaves an existing file byte-identical.
        CloudProviderRegistry registry;
        registry.install("fireworks", "https://api.fireworks.ai/inference/v1");
        const json array = registry.to_config_array();
        r.check(array.size() == 1 && !array[0].contains("auth_header_name") &&
                    !array[0].contains("auth_header_prefix"),
                "default auth header is omitted from the serialized config");
    }

    {
        // An old config.json written before these fields existed has no
        // auth_header_* keys at all -- must fall back to the previously
        // hardcoded Bearer behavior, not an empty header.
        CloudProviderRegistry registry;
        json legacy_array = json::array({
            {{"name", "openai"}, {"base_url", "https://api.openai.com/v1"}}
        });
        registry.load_from_config(legacy_array);
        const auto header = registry.auth_header_for("openai");
        r.check(header.name == "Authorization",
                "legacy config entry missing header fields -> defaults to Authorization");
        r.check(header.prefix == "Bearer ",
                "legacy config entry missing header fields -> defaults to 'Bearer '");
    }

    {
        CloudProviderRegistry registry;
        const auto header = registry.auth_header_for("not-installed");
        r.check(header.name == "Authorization" && header.prefix == "Bearer ",
                "auth_header_for() on unknown provider -> default header");
    }

    {
        // Re-installing with identical options reports "unchanged" so callers
        // can skip a config write; changing only the header must report a change.
        CloudProviderRegistry::InstallOptions options;
        options.auth_header_name = "X-Api-Key";
        options.auth_header_prefix = "";

        CloudProviderRegistry registry;
        registry.install("acme", "https://gateway.example.com/v1", options);
        r.check(!registry.install("acme", "https://gateway.example.com/v1", options),
                "re-install with identical options -> reports no change");

        options.auth_header_name = "X-Other-Key";
        r.check(registry.install("acme", "https://gateway.example.com/v1", options),
                "re-install with a changed header name -> reports a change");
        r.check(registry.auth_header_for("acme").name == "X-Other-Key",
                "re-install with a changed header name -> new value stored");
    }

    {
        // The insecure-http opt-in path must not disturb the auth header: it
        // is a separate setter precisely so a partial update cannot silently
        // reset unrelated fields back to their defaults.
        CloudProviderRegistry::InstallOptions options;
        options.auth_header_name = "X-Api-Key";
        options.auth_header_prefix = "";

        CloudProviderRegistry registry;
        registry.install("acme", "http://gateway.example.com/v1", options);
        r.check(registry.set_allow_insecure_http("acme", true),
                "set_allow_insecure_http() on an installed provider -> true");
        r.check(registry.allow_insecure_http_for("acme"),
                "set_allow_insecure_http() -> flag is set");

        const auto header = registry.auth_header_for("acme");
        r.check(header.name == "X-Api-Key" && header.prefix.empty(),
                "set_allow_insecure_http() preserves the custom auth header");
        r.check(!registry.set_allow_insecure_http("not-installed", true),
                "set_allow_insecure_http() on unknown provider -> false");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
