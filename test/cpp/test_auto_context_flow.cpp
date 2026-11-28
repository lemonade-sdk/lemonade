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
                       bool safer, bool bounded_swa = false,
                       double kv_bytes_ratio = 1.0) {
    return compute_auto_context_size(info, free_gib, false, false, -1.0,
                                     bounded_swa, safer, true, kv_bytes_ratio);
}

int main() {
    // With measurable weight fit, default mode deliberately tries 4k even if
    // reserved KV suggests 1200 tokens. Safer opt-in rejects that shortfall.
    const auto short_ctx = fixture("llamacpp", 3.0, DEVICE_GPU);
    CHECK(resolve(short_ctx, 4.646484375, false) == 4096);
    CHECK(resolve(short_ctx, 4.646484375, true) == AUTO_CTX_NO_CAPACITY);

    // A positive measurement remains exact for non-strict recipes, while
    // llama.cpp GPU retains the intentional best-effort 4k exception above.
    auto short_flm = fixture("flm", 3.0, DEVICE_CPU);
    CHECK(resolve(short_flm, 4.646484375, false) == 1200);
    CHECK(resolve(short_flm, 4.646484375, true) == AUTO_CTX_NO_CAPACITY);
    auto short_cpu_llama = fixture("llamacpp", 3.0, DEVICE_CPU);
    CHECK(resolve(short_cpu_llama, 4.646484375, false) == 1200);

    // These were best-effort before PR #3988. The estimate must not turn
    // into a blanket load rejection unless stricter admission was requested.
    auto flm = fixture("flm", 20.0, DEVICE_CPU);
    CHECK(resolve(flm, 4.0, false) == 4096);
    CHECK(resolve(flm, 4.0, true) == AUTO_CTX_NO_CAPACITY);
    flm.max_context_window = 2048;
    CHECK(resolve(flm, 4.0, false) == 2048);

    // CPU-only llama.cpp can use host paging; the hard GPU admission must not
    // reject its old best-effort minimum by default.
    auto cpu_llama = fixture("llamacpp", 20.0, DEVICE_CPU);
    CHECK(resolve(cpu_llama, 4.0, false) == 4096);
    CHECK(resolve(cpu_llama, 4.0, true) == AUTO_CTX_NO_CAPACITY);

    // The effective backend wins over llamacpp's GPU catalog default.
    const auto requested_gpu = fixture("llamacpp", 6.0, DEVICE_GPU);
    CHECK(auto_context_device(requested_gpu, requested_gpu.recipe, "cpu", "") == DEVICE_CPU);
    CHECK(auto_context_device(requested_gpu, requested_gpu.recipe, "cuda", "") == DEVICE_GPU);
    CHECK(auto_context_device(requested_gpu, requested_gpu.recipe, "rocm", "") == DEVICE_GPU);
    const auto other_recipe = fixture("ds4", 6.0, DEVICE_GPU);
    CHECK(auto_context_device(other_recipe, other_recipe.recipe, "cpu", "") == DEVICE_GPU);
    auto selected_cpu = requested_gpu;
    selected_cpu.device = auto_context_device(selected_cpu, selected_cpu.recipe, "cpu", "");
    CHECK(resolve(requested_gpu, 2.0, true) == AUTO_CTX_NO_CAPACITY);
    CHECK(resolve(selected_cpu, 20.0, true) >= 4096);

    auto host_placed = requested_gpu;
    const auto host_placement = auto_context_llamacpp_placement(
        "-ngl 0 -nkvo", nullptr, nullptr, nullptr, false);
    host_placed.device = auto_context_device(host_placed, host_placed.recipe,
                                           "vulkan", "", host_placement.host_only);
    CHECK(host_placed.device == DEVICE_CPU);
    CHECK(resolve(host_placed, 20.0, true) >= 4096);
    CHECK(auto_context_device(requested_gpu, requested_gpu.recipe, "vulkan", "",
        auto_context_llamacpp_placement("-ngl 0", nullptr, nullptr, nullptr, false).host_only)
        == DEVICE_GPU);

    // Rule-head admission must test its actual 2048-token window (not the
    // GGUF base model's 128k context, nor a generic 4096-token floor).
    auto rule_head = fixture("llamacpp", 3.0, DEVICE_GPU);
    rule_head.max_context_window = backends::llamacpp::RULE_HEAD_ROUTER_CTX_SIZE;
    CHECK(resolve(rule_head, 16.0, true) == rule_head.max_context_window);
    CHECK(resolve(rule_head, 4.0, true) == AUTO_CTX_NO_CAPACITY);

    // DS4 owns its resident working-set budget. A real, non-empty file
    // ensures this exercises the backend's non-null KV-budget hook instead
    // of silently testing its generic nullopt path.
    const auto ds4_file = std::filesystem::temp_directory_path() /
        ("lemonade-ds4-ctx-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream out(ds4_file, std::ios::binary);
        out << "test-gguf-weights";
    }
    CHECK(std::filesystem::file_size(ds4_file) > 0);
    auto ds4 = fixture("ds4", 20.0, DEVICE_GPU);
    ds4.min_resident_gb = 10.0;
    ds4.resolved_paths["main"] = ds4_file.string();
    CHECK(resolve(ds4, 1e-8, false) == 4096);
    CHECK(resolve(ds4, 1e-8, true) == AUTO_CTX_NO_CAPACITY);
    // The backend's working-set estimate permits KV here although the full
    // checkpoint cannot fit. A generic llama.cpp calculation must reject it.
    CHECK(resolve(ds4, 20.0, true) >= 4096);
    auto generic = ds4;
    generic.recipe = "llamacpp";
    CHECK(resolve(generic, 20.0, true) == AUTO_CTX_NO_CAPACITY);
    std::filesystem::remove(ds4_file);

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
    const auto full_kv = resolve(gemma, 31.8, false, false);
    const auto bounded_kv = resolve(gemma, 31.8, false, true);
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
    CHECK(resolve(flint, 31.8, false, false) < 131072);
    CHECK(resolve(flint, 31.8, false, true) == 262144);

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
    CHECK(resolve(qwen, 31.8, false, false) < 262144);
    CHECK(resolve(qwen, 31.8, false, false, q8) == 262144);

    // The registry device is GPU for every llama.cpp model, including CPU runs.
    const auto gpu_model = fixture("llamacpp", 3.0, DEVICE_GPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "cpu", "") == DEVICE_CPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "vulkan", "none") == DEVICE_CPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "vulkan", "") == DEVICE_GPU);
    CHECK(auto_context_device(gpu_model, "llamacpp", "rocm", "ROCm0") == DEVICE_GPU);
    CHECK(auto_context_llamacpp_device("llamacpp", "", "none") == "none");
    CHECK(auto_context_llamacpp_device("llamacpp", "auto", "none") == "none");
    CHECK(auto_context_llamacpp_device("llamacpp", "Vulkan0", "none") == "Vulkan0");
    CHECK(auto_context_llamacpp_device("llamacpp", "", nullptr).empty());
    CHECK(auto_context_llamacpp_device("vllm", "", "none").empty());
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
