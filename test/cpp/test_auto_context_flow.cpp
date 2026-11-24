#include <lemon/auto_tune.h>

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace lemon;

static int failures = 0;
static void check(bool passed, const char* expression, int line) {
    if (!passed) {
        std::fprintf(stderr, "%d: %s failed\n", line, expression);
        ++failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

static ModelInfo fixture(const char* recipe, double size_gib, DeviceType device) {
    ModelInfo info;
    info.model_name = "auto-context-test";
    info.recipe = recipe;
    info.size = size_gib;
    info.device = device;
    info.max_context_window = 32768;
    info.gguf.architecture = "llama";
    info.gguf.block_count = 32;
    info.gguf.key_length = 128;
    info.gguf.value_length = 128;
    info.gguf.head_count_kv = 256;
    return info;
}

static int64_t resolve(const ModelInfo& info, double free_gib,
                       bool bounded_swa = false,
                       double kv_bytes_ratio = 1.0) {
    return compute_auto_context_size(info, free_gib, false, false, -1.0,
                                     bounded_swa, kv_bytes_ratio);
}

int main() {
    // A positive estimate must not be increased to the 4k load-policy floor.
    // 3 GiB weights + 1.5 GiB reserves + 1200 * 128 KiB KV = 4.646484375 GiB.
    const auto short_ctx = fixture("llamacpp", 3.0, DEVICE_GPU);
    CHECK(resolve(short_ctx, 4.646484375) == 1200);

    // The actual production estimator must still exploit Gemma4's bounded
    // KV cache rather than charge all sliding layers for every token.
    auto gemma = fixture("llamacpp", 7.75, DEVICE_GPU);
    gemma.model_name = "Gemma4-test-GGUF";
    gemma.max_context_window = 262144;
    gemma.labels = {"mtp"};
    gemma.gguf.architecture = "gemma4";
    gemma.gguf.block_count = 48;
    gemma.gguf.key_length = 512;
    gemma.gguf.value_length = 512;
    gemma.gguf.key_length_swa = 256;
    gemma.gguf.value_length_swa = 256;
    gemma.gguf.sliding_window = 1024;
    gemma.gguf.sliding_window_pattern.clear();
    gemma.gguf.head_count_kv_per_layer.clear();
    gemma.gguf.head_count_kv = 0;
    for (int i = 0; i < 48; ++i) {
        const bool sliding = i % 6 != 5;
        const int heads = sliding ? 8 : 1;
        gemma.gguf.sliding_window_pattern.push_back(sliding);
        gemma.gguf.head_count_kv_per_layer.push_back(heads);
        gemma.gguf.head_count_kv += heads;
    }
    const auto full_kv = resolve(gemma, 31.8, false);
    const auto bounded_kv = resolve(gemma, 31.8, true);
    CHECK(full_kv > 0 && full_kv < 100000);
    CHECK(bounded_kv == 262144);

    // Flint: 35 SWA / 14 dense layers, explicit GGUF pattern, 513-cell window.
    // The old unbounded curve artificially limits context on a 32 GiB card.
    auto flint = fixture("llamacpp", 20.0, DEVICE_GPU);
    flint.gguf.architecture = "flint";
    flint.max_context_window = 262144;
    flint.gguf.block_count = 49;
    flint.gguf.key_length = 128;
    flint.gguf.value_length = 128;
    flint.gguf.sliding_window = 513;
    flint.gguf.head_count_kv = 196;
    flint.gguf.head_count_kv_per_layer.assign(49, 4);
    flint.gguf.sliding_window_pattern.assign(49, true);
    for (int i = 0; i < 14; ++i)
        flint.gguf.sliding_window_pattern[static_cast<size_t>(i * 3)] = false;
    CHECK(resolve(flint, 31.8, false) < 131072);
    CHECK(resolve(flint, 31.8, true) == 262144);

    // Qwen3.8 27B: 16 full-attention groups, MTP, Q8 K/V. The format flags
    // must affect the runtime estimator, not just the llama-server invocation.
    auto qwen = fixture("llamacpp", 17.0, DEVICE_GPU);
    qwen.max_context_window = 262144;
    qwen.gguf.block_count = 65;
    qwen.gguf.head_count_kv = 260;
    qwen.gguf.key_length = 256;
    qwen.gguf.value_length = 256;
    qwen.gguf.full_attention_interval = 4;
    qwen.labels = {"mtp"};
    const double q8 = auto_context_llamacpp_kv_bytes_ratio(
        "llamacpp", "--chat-template-kwargs '{\"preserve_thinking\":true}' "
        "--parallel 1 --spec-type draft-mtp -ctk q8_0 -ctv q8_0 -fa on",
        nullptr, nullptr);
    CHECK(q8 == 17.0 / 32.0);
    CHECK(resolve(qwen, 31.8, false) < 262144);
    CHECK(resolve(qwen, 31.8, false, q8) == 262144);

    // The registry device is GPU for every llama.cpp model, including CPU runs.
    const auto gpu_model = fixture("llamacpp", 3.0, DEVICE_GPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "cpu", "") == DEVICE_CPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "vulkan", "none") == DEVICE_CPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "vulkan", "") == DEVICE_GPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "rocm", "ROCm0") == DEVICE_GPU);
    const auto npu_model = fixture("flm", 3.0, DEVICE_NPU);
    CHECK(auto_context_device(npu_model, "flm", "", "") == DEVICE_NPU);

    // Recipes with uses_ctx_size=false (such as loom and cloud) manage their
    // own context window; auto-tune must not run or inject a synthetic ctx_size.
    for (const char* recipe : {"loom", "cloud"}) {
        ModelInfo fixed_ctx;
        fixed_ctx.model_name = std::string("fixed-ctx-") + recipe;
        fixed_ctx.recipe = recipe;
        fixed_ctx.size = 19.3;
        fixed_ctx.device = DEVICE_GPU;
        fixed_ctx.max_context_window = 262144;
        RecipeOptions opts(recipe, nlohmann::json::object());
        CHECK(resolve_auto_ctx_size(opts, fixed_ctx) == -2);
    }

    return failures ? 1 : 0;
}
