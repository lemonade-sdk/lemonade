#include "lemon/utils/origin_utils.h"

#include <cstdio>
#include <string>
#include <vector>

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

static void test_implicit_allowances(TestResult& r) {
    using namespace lemon::utils;
    const std::vector<std::string> none;

    r.check(!is_origin_allowed("", none), "reject empty origin");

    r.check(is_origin_allowed("http://localhost:3000", none), "allow http loopback host");
    r.check(is_origin_allowed("http://127.0.0.1:8080", none), "allow 127.0.0.1");
    r.check(is_origin_allowed("http://[::1]:3000", none), "allow [::1]");
    r.check(is_origin_allowed("ws://localhost", none), "allow ws loopback");

    r.check(is_origin_allowed("tauri://localhost", none), "allow tauri scheme");
    r.check(is_origin_allowed("http://tauri.localhost", none), "allow WebView2 virtual host");

    // Non-web schemes require local filesystem access or a desktop wrapper, so a
    // remote page cannot forge them.
    r.check(is_origin_allowed("file://", none), "allow file scheme");
    r.check(is_origin_allowed("jan://app", none), "allow custom app scheme");

    r.check(!is_origin_allowed("http://evil.example", none),
            "reject foreign http origin without allow-list");
    r.check(!is_origin_allowed("https://192.168.1.50:13305", none),
            "reject non-loopback LAN origin without allow-list");
}

static void test_configured_allowances(TestResult& r) {
    using namespace lemon::utils;

    const std::vector<std::string> lan = {"http://192.168.1.50:13305"};
    r.check(is_origin_allowed("http://192.168.1.50:13305", lan),
            "allow exactly-configured non-loopback origin");
    r.check(!is_origin_allowed("https://192.168.1.50:13305", lan),
            "reject scheme mismatch against configured origin");
    r.check(!is_origin_allowed("http://192.168.1.50:9999", lan),
            "reject port mismatch against configured origin");

    const std::vector<std::string> star = {"*"};
    r.check(is_origin_allowed("https://app.example.com", star), "wildcard allows any origin");
    r.check(is_origin_allowed("http://evil.example", star), "wildcard allows foreign origin");
    r.check(!is_origin_allowed("", star), "wildcard still rejects empty origin");

    const std::vector<std::string> multi = {
        "http://another.com", "https://app.example.com"};
    r.check(is_origin_allowed("https://app.example.com", multi),
            "match later entry in allow-list");
    r.check(!is_origin_allowed("https://not-listed.com", multi),
            "reject origin absent from allow-list");
}

static void test_opaque_null(TestResult& r) {
    using namespace lemon::utils;

    r.check(!is_origin_allowed("null", {}), "reject opaque null without allow-list");
    r.check(is_origin_allowed("null", {"null"}), "allow opaque null when allow-listed");

    // A web origin whose host is literally "null" must never be granted by a
    // "null" allow-list entry.
    const std::vector<std::string> null_allow = {"null"};
    r.check(!is_origin_allowed("http://null", null_allow),
            "reject http://null against null allow-list");
    r.check(!is_origin_allowed("https://null", null_allow),
            "reject https://null against null allow-list");
}

int main() {
    TestResult r;
    printf("=== OriginUtils Unit Tests ===\n\n");

    test_implicit_allowances(r);
    test_configured_allowances(r);
    test_opaque_null(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
