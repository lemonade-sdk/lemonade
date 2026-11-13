// backend_source: where pinned backend release files are downloaded from.
// Covers the URL builders in lemon/backends/backend_source.h and the
// backend_source config key (default, env override, validation).
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <lemon/backends/backend_source.h>
#include <lemon/runtime_config.h>

using json = nlohmann::json;
using lemon::RuntimeConfig;
namespace bs = lemon::backend_source;

static int passed = 0;
static int failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("[PASS] %s\n", desc);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", desc);
        ++failures;
    }
}

static void check_eq(const std::string& got, const std::string& want, const char* desc) {
    check(got == want, desc);
    if (got != want) {
        std::printf("       got:  %s\n       want: %s\n", got.c_str(), want.c_str());
    }
}

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

int main() {
    // GitHub is unchanged and stays the default.
    check_eq(bs::release_asset_url("github", "", "ggml-org/llama.cpp", "b12109",
                                   "llama-b12109-bin-win-vulkan-x64.zip"),
             "https://github.com/ggml-org/llama.cpp/releases/download/b12109/"
             "llama-b12109-bin-win-vulkan-x64.zip",
             "github release asset URL is the GitHub Releases download URL");
    check_eq(bs::therock_tarball_url("github", "", "10.1.0",
                                     "therock-dist-windows-gfx1151-10.1.0.tar.gz"),
             "https://stable.repo.amd.com/rocm/core/tarball/therock-dist-windows-gfx1151-10.1.0.tar.gz",
             "github TheRock URL is AMD's tarball server");

    // ModelScope keeps the GitHub file name under github/<repo>/<tag>/.
    check_eq(bs::release_asset_url("modelscope", "", "ggml-org/llama.cpp", "b12109",
                                   "llama-b12109-bin-win-vulkan-x64.zip"),
             "https://modelscope.cn/api/v1/datasets/lemonade-sdk/backends/repo?Revision=master"
             "&FilePath=github%2Fggml-org%2Fllama.cpp%2Fb12109%2Fllama-b12109-bin-win-vulkan-x64.zip",
             "modelscope release asset URL points at the mirrored path");
    check_eq(bs::release_asset_url("modelscope", "", "lemonade-sdk/vllm-rocm",
                                   "vllm0.20.1-rocm7.12.0-gfx1151",
                                   "vllm0.20.1-rocm7.12.0-gfx1151-x64.partcount"),
             "https://modelscope.cn/api/v1/datasets/lemonade-sdk/backends/repo?Revision=master"
             "&FilePath=github%2Flemonade-sdk%2Fvllm-rocm%2Fvllm0.20.1-rocm7.12.0-gfx1151%2F"
             "vllm0.20.1-rocm7.12.0-gfx1151-x64.partcount",
             "modelscope partcount probe uses the same layout");
    check_eq(bs::release_manifest_url("", "sd-cpp/x", "master-1-abc"),
             "https://modelscope.cn/api/v1/datasets/lemonade-sdk/backends/repo?Revision=master"
             "&FilePath=github%2Fsd-cpp%2Fx%2Fmaster-1-abc%2Fassets.json",
             "modelscope manifest URL is assets.json in the release folder");
    check_eq(bs::therock_tarball_url("modelscope", "", "10.1.0",
                                     "therock-dist-linux-gfx110X-all-10.1.0.tar.gz"),
             "https://modelscope.cn/api/v1/datasets/lemonade-sdk/backends/repo?Revision=master"
             "&FilePath=therock%2F10.1.0%2Ftherock-dist-linux-gfx110X-all-10.1.0.tar.gz",
             "modelscope TheRock URL points at therock/<version>/");
    check_eq(bs::release_asset_url("modelscope", "https://www.modelscope.ai/", "a/b", "v1", "f.zip"),
             "https://www.modelscope.ai/api/v1/datasets/lemonade-sdk/backends/repo?Revision=master"
             "&FilePath=github%2Fa%2Fb%2Fv1%2Ff.zip",
             "MODELSCOPE_ENDPOINT override is honoured and its trailing slash dropped");
    check_eq(bs::reachability_probe_url("modelscope", ""), "https://modelscope.cn/",
             "modelscope reachability probe targets ModelScope");
    check_eq(bs::reachability_probe_url("github", ""), "https://github.com/",
             "github reachability probe targets GitHub");

    // Config key: default, accepted values, rejected values, env override.
    set_env("LEMONADE_BACKEND_SOURCE", nullptr);
    {
        RuntimeConfig config(json::object());
        check(config.backend_source() == "github", "missing backend_source defaults to github");
    }
    {
        RuntimeConfig config(json{{"backend_source", "github"}});
        config.set(json{{"backend_source", "modelscope"}});
        check(config.backend_source() == "modelscope", "backend_source can be set to modelscope");
        check(config.snapshot()["backend_source"] == "modelscope", "modelscope reflected in snapshot");
    }
    {
        RuntimeConfig config(json{{"backend_source", "github"}});
        bool threw = false;
        try {
            config.set(json{{"backend_source", "gitee"}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "unknown backend_source is rejected");
        check(config.backend_source() == "github", "rejected value leaves backend_source unchanged");
    }
    {
        RuntimeConfig config(json{{"backend_source", "github"}});
        bool threw = false;
        try {
            config.set(json{{"backend_source", true}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "non-string backend_source is rejected");
    }
    {
        set_env("LEMONADE_BACKEND_SOURCE", "modelscope");
        RuntimeConfig config(json{{"backend_source", "github"}});
        check(config.backend_source() == "modelscope", "LEMONADE_BACKEND_SOURCE overrides the config file");
        set_env("LEMONADE_BACKEND_SOURCE", nullptr);
    }

    std::printf("\n%d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
