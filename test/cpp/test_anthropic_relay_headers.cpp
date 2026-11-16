// Unit tests for the /v1/messages relay header allowlists.
//
// The relay rebuilds the request envelope rather than copying it, so these
// predicates are the only thing keeping a client's feature opt-in — or a
// provider's rate-limit window — from being silently dropped on the hop. The
// exclusions matter as much as the inclusions: forwarding a client's
// Authorization or the upstream Content-Length would break the relay outright.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.

#include "lemon/anthropic_relay_headers.h"

#include <cstdio>
#include <string>

using lemon::anthropic::is_forwardable_request_header;
using lemon::anthropic::is_forwardable_response_header;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        printf("[%s] %s\n", condition ? "PASS" : "FAIL", name.c_str());
        if (condition) {
            ++passed;
        } else {
            ++failed;
        }
    }
};

static void test_request_headers(TestResult& r) {
    r.check(is_forwardable_request_header("anthropic-beta"),
            "anthropic-beta forwarded (SDK feature opt-ins)");
    r.check(is_forwardable_request_header("anthropic-version"),
            "anthropic-version forwarded (client pin beats the default)");

    // The relay sets its own auth from the provider's configured header; a
    // client value must never reach upstream.
    r.check(!is_forwardable_request_header("authorization"),
            "authorization not forwarded");
    r.check(!is_forwardable_request_header("x-api-key"),
            "x-api-key not forwarded");

    // The body is re-serialized after the model rewrite, so a copied
    // Content-Length would describe the wrong body.
    r.check(!is_forwardable_request_header("content-length"),
            "content-length not forwarded");
    r.check(!is_forwardable_request_header("host"), "host not forwarded");
    r.check(!is_forwardable_request_header("connection"),
            "hop-by-hop connection not forwarded");
    r.check(!is_forwardable_request_header("content-type"),
            "content-type not forwarded (relay sets application/json)");

    // The predicate takes an already-lowercased name; a caller that forgets to
    // normalize should miss rather than match by accident.
    r.check(!is_forwardable_request_header("Anthropic-Beta"),
            "match is exact on the lowercased name");
    r.check(!is_forwardable_request_header("anthropic-beta-extra"),
            "no prefix match on request names");
    r.check(!is_forwardable_request_header(""), "empty name not forwarded");
}

static void test_response_headers(TestResult& r) {
    r.check(is_forwardable_response_header("retry-after"),
            "retry-after relayed (SDK backoff)");
    r.check(is_forwardable_response_header("request-id"),
            "request-id relayed (escalation to the provider)");

    // Prefix match: the family is open-ended, so naming each member would rot.
    r.check(is_forwardable_response_header("anthropic-ratelimit-requests-reset"),
            "anthropic-ratelimit-requests-reset relayed");
    r.check(is_forwardable_response_header("anthropic-ratelimit-tokens-remaining"),
            "anthropic-ratelimit-tokens-remaining relayed");
    r.check(is_forwardable_response_header("anthropic-ratelimit-"),
            "bare anthropic-ratelimit- prefix matches");

    r.check(!is_forwardable_response_header("anthropic-ratelimi"),
            "partial prefix does not match");
    r.check(!is_forwardable_response_header("x-anthropic-ratelimit-requests-reset"),
            "prefix is anchored at the start of the name");

    // Relaying these would conflict with what httplib writes for the chunked
    // SSE response the relay actually sends.
    r.check(!is_forwardable_response_header("content-length"),
            "content-length not relayed");
    r.check(!is_forwardable_response_header("transfer-encoding"),
            "transfer-encoding not relayed");
    r.check(!is_forwardable_response_header("content-type"),
            "content-type not relayed");
    r.check(!is_forwardable_response_header("set-cookie"),
            "set-cookie not relayed");
    r.check(!is_forwardable_response_header(""), "empty name not relayed");
}

int main() {
    TestResult r;
    printf("=== Anthropic Relay Header Allowlist Unit Tests ===\n\n");

    test_request_headers(r);
    test_response_headers(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
