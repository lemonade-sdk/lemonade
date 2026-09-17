#include <lemon/utils/github_api.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
    #define set_env(k, v) _putenv_s(k, v)
    #define unset_env(k)  _putenv_s(k, "")
#else
    static void set_env(const char* k, const char* v) {
        setenv(k, v ? v : "", 1);
    }
    #define unset_env(k) unsetenv(k)
#endif

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

int main() {
    TestResult result;

    const std::string kUserAgent{"User-Agent"};
    const std::string kAccept{"Accept"};
    const std::string kAuth{"Authorization"};

    // Save and clear both env vars so tests start from a known state.
    const char* saved_github = std::getenv("GITHUB_TOKEN");
    const char* saved_gh     = std::getenv("GH_TOKEN");
    const std::string orig_github = saved_github ? saved_github : "";
    const std::string orig_gh     = saved_gh     ? saved_gh     : "";

    // 1. Neither GITHUB_TOKEN nor GH_TOKEN set — no Authorization header.
    {
        unset_env("GITHUB_TOKEN");
        unset_env("GH_TOKEN");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kAuth) == 0 &&
            h.at(kUserAgent) == "lemonade" &&
            h.count(kAccept) > 0) {
            result.ok("no token env vars");
        } else {
            result.fail("no token env vars");
        }
    }

    // 2. GITHUB_TOKEN set — Authorization uses it.
    {
        set_env("GITHUB_TOKEN", "tok-ghub");
        unset_env("GH_TOKEN");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kAuth) == 1 &&
            h.at(kAuth) == "Bearer tok-ghub") {
            result.ok("GITHUB_TOKEN set");
        } else {
            result.fail("GITHUB_TOKEN set");
        }
    }

    // 3. Only GH_TOKEN set — Authorization uses it.
    {
        unset_env("GITHUB_TOKEN");
        set_env("GH_TOKEN", "tok-gh");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kAuth) == 1 &&
            h.at(kAuth) == "Bearer tok-gh") {
            result.ok("only GH_TOKEN set");
        } else {
            result.fail("only GH_TOKEN set");
        }
    }

    // 4. GITHUB_TOKEN empty, GH_TOKEN set — falls back to GH_TOKEN.
    {
        set_env("GITHUB_TOKEN", "");  // present but empty
        set_env("GH_TOKEN", "tok-gh");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kAuth) == 1 &&
            h.at(kAuth) == "Bearer tok-gh") {
            result.ok("empty GITHUB_TOKEN falls back to GH_TOKEN");
        } else {
            result.fail("empty GITHUB_TOKEN falls back to GH_TOKEN");
        }
    }

    // 5. Both set — GITHUB_TOKEN takes priority.
    {
        set_env("GITHUB_TOKEN", "tok-ghub");
        set_env("GH_TOKEN", "tok-gh");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kAuth) == 1 &&
            h.at(kAuth) == "Bearer tok-ghub") {
            result.ok("both set, GITHUB_TOKEN wins");
        } else {
            result.fail("both set, GITHUB_TOKEN wins");
        }
    }

    // 6. Baseline headers are always present (no tokens).
    {
        unset_env("GITHUB_TOKEN");
        unset_env("GH_TOKEN");
        auto h = lemon::utils::github_api::headers();
        if (h.count(kUserAgent) == 1 && h.at(kUserAgent) == "lemonade" &&
            h.count(kAccept) == 1    && h.at(kAccept) == "application/vnd.github+json") {
            result.ok("baseline headers present");
        } else {
            result.fail("baseline headers present");
        }
    }

    // Restore original environment.
    if (orig_github.empty())
        unset_env("GITHUB_TOKEN");
    else
        set_env("GITHUB_TOKEN", orig_github.c_str());

    if (orig_gh.empty())
        unset_env("GH_TOKEN");
    else
        set_env("GH_TOKEN", orig_gh.c_str());

    printf("\n%d passed, %d failed\n", result.passed, result.failed);
    return result.failed > 0 ? 1 : 0;
}
