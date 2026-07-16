#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/recipe_options.h"

#include <cstdio>
#include <string>

using lemon::DEVICE_CPU;
using lemon::DEVICE_GPU;
using lemon::DEVICE_NPU;
using lemon::RecipeOptions;
using lemon::backends::LlamaCppServer;
using lemon::backends::WhisperServer;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static RecipeOptions options_with_backend(const std::string& key, const std::string& value) {
    RecipeOptions options;
    options.set_option(key, value);
    return options;
}

static void test_llamacpp_effective_device() {
    LlamaCppServer server("info", nullptr, nullptr);

    check("llamacpp cpu backend resolves to DEVICE_CPU",
          server.effective_device(options_with_backend("llamacpp_backend", "cpu")) == DEVICE_CPU);
    check("llamacpp system backend resolves to DEVICE_CPU",
          server.effective_device(options_with_backend("llamacpp_backend", "system")) == DEVICE_CPU);
    check("llamacpp vulkan backend resolves to DEVICE_GPU",
          server.effective_device(options_with_backend("llamacpp_backend", "vulkan")) == DEVICE_GPU);
    check("llamacpp rocm backend resolves to DEVICE_GPU",
          server.effective_device(options_with_backend("llamacpp_backend", "rocm")) == DEVICE_GPU);

    check("llamacpp rocm backend is AMD GPU",
          server.effective_is_amd_gpu(options_with_backend("llamacpp_backend", "rocm")));
    check("llamacpp cuda backend is NOT AMD GPU",
          !server.effective_is_amd_gpu(options_with_backend("llamacpp_backend", "cuda")));
    check("llamacpp vulkan backend is NOT AMD GPU (vendor-neutral, conservatively excluded)",
          !server.effective_is_amd_gpu(options_with_backend("llamacpp_backend", "vulkan")));
    check("llamacpp cpu backend is NOT AMD GPU",
          !server.effective_is_amd_gpu(options_with_backend("llamacpp_backend", "cpu")));
}

static void test_whispercpp_effective_device() {
    WhisperServer server("info", nullptr, nullptr);

    check("whispercpp cpu backend resolves to DEVICE_CPU",
          server.effective_device(options_with_backend("whispercpp_backend", "cpu")) == DEVICE_CPU);
    check("whispercpp npu backend resolves to DEVICE_NPU",
          server.effective_device(options_with_backend("whispercpp_backend", "npu")) == DEVICE_NPU);
    check("whispercpp rocm backend resolves to DEVICE_GPU (previously misclassified as CPU)",
          server.effective_device(options_with_backend("whispercpp_backend", "rocm")) == DEVICE_GPU);
    check("whispercpp vulkan backend resolves to DEVICE_GPU",
          server.effective_device(options_with_backend("whispercpp_backend", "vulkan")) == DEVICE_GPU);

    check("whispercpp rocm backend is AMD GPU",
          server.effective_is_amd_gpu(options_with_backend("whispercpp_backend", "rocm")));
    check("whispercpp vulkan backend is NOT AMD GPU (vendor-neutral, conservatively excluded)",
          !server.effective_is_amd_gpu(options_with_backend("whispercpp_backend", "vulkan")));
    check("whispercpp npu backend is NOT AMD GPU",
          !server.effective_is_amd_gpu(options_with_backend("whispercpp_backend", "npu")));
}

int main() {
    test_llamacpp_effective_device();
    test_whispercpp_effective_device();

    if (g_failures == 0) {
        std::printf("All backend effective-device tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
