// Regression tests for CloudServer public-model restoration on JSON and SSE
// frames (lemonade-sdk/lemonade#2964 / PR #2980).
//
// Uses an explicit pass/fail counter (not assert()) so the test stays
// effective under Release builds where -DNDEBUG would compile assert away.

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include <lemon/backends/cloud/cloud_server.h>

using json = nlohmann::json;
using lemon::backends::CloudServer;

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
    printf("=== CloudServer response model Unit Tests ===\n\n");

    const std::string public_model = "my-public-alias";

    // Non-streaming: provider echoes upstream id -> restore public name.
    {
        json response = {{"id", "chatcmpl-1"},
                         {"model", "accounts/fireworks/models/upstream-id"},
                         {"choices", json::array()}};
        json restored = CloudServer::restore_public_model(response, public_model);
        r.check(restored.value("model", "") == public_model,
                "non-streaming model == public_model");
    }

    // Streaming SSE data frame: rewrite model field.
    {
        std::string line =
            "data: {\"model\":\"accounts/fireworks/models/upstream-id\",\"choices\":[]}";
        std::string out = CloudServer::rewrite_sse_model_line(line, public_model);
        r.check(out.rfind("data: ", 0) == 0, "streaming rewrite keeps data: prefix");
        try {
            json chunk = json::parse(out.substr(6));
            r.check(chunk.value("model", "") == public_model,
                    "streaming model == public_model");
        } catch (const json::exception&) {
            r.check(false, "streaming model == public_model");
        }
    }

    // [DONE] and non-JSON frames are left alone.
    {
        r.check(CloudServer::rewrite_sse_model_line("data: [DONE]", public_model) ==
                    "data: [DONE]",
                "rewrite leaves [DONE] unchanged");
        r.check(CloudServer::rewrite_sse_model_line(": keepalive", public_model) ==
                    ": keepalive",
                "rewrite leaves non-data frames unchanged");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
