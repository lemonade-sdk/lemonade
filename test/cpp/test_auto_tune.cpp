// Standalone test for GGUF array storage and weighted KV cache computation.
//
// Compile: g++ -std=c++17 -I src/cpp/include test/cpp/test_auto_tune.cpp -o test_auto_tune

#include "lemon/gguf_reader.h"
#include "lemon/swa_cache_profile.h"
#include "lemon/auto_context_budget.h"
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>
#include <cstdio>
#include <limits>
#include <vector>

using lemon::GgufMetadata;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool approx_eq(double a, double b, double tol = 0.001) {
    return std::fabs(a - b) < tol;
}

/// Simulate what read_gguf_metadata does after the KV loop:
/// derive head_count_kv and swa_layer_count from raw arrays/scalars.
static void derive_scalars(GgufMetadata& m) {
    if (!m.head_count_kv_per_layer.empty()) {
        for (int64_t v : m.head_count_kv_per_layer)
            m.head_count_kv += v;
    } else if (m.head_count_kv_scalar > 0 && m.block_count > 0) {
        m.head_count_kv = m.head_count_kv_scalar * m.block_count;
        m.head_count_kv_per_layer.assign(m.block_count, m.head_count_kv_scalar);
    }

    if (!m.sliding_window_pattern.empty()) {
        for (bool v : m.sliding_window_pattern)
            if (v) m.swa_layer_count++;
    }
}

static void test_scalar_head_count_kv() {
    GgufMetadata m;
    m.block_count = 32;
    m.head_count_kv_scalar = 4;

    derive_scalars(m);

    check("scalar: head_count_kv = scalar * block_count",
          m.head_count_kv == 4 * 32);
    check("scalar: per_layer array populated uniformly",
          m.head_count_kv_per_layer.size() == 32);
    check("scalar: per_layer values all equal scalar",
          std::all_of(m.head_count_kv_per_layer.begin(),
                      m.head_count_kv_per_layer.end(),
                      [](int64_t v) { return v == 4; }));
}

static void test_array_head_count_kv() {
    GgufMetadata m;
    m.block_count = 4;
    m.head_count_kv_per_layer = {8, 16, 8, 16};  // varying per block

    derive_scalars(m);

    check("array: head_count_kv = sum of array",
          m.head_count_kv == 8 + 16 + 8 + 16);
    check("array: scalar not used when array present",
          m.head_count_kv == 48);
}

static void test_swa_pattern_derivation() {
    GgufMetadata m;
    m.block_count = 8;
    m.sliding_window_pattern = {false, true, false, true, false, true, false, true};

    derive_scalars(m);

    check("swa: swa_layer_count = number of true entries",
          m.swa_layer_count == 4);
}

static void test_standard_mha() {
    GgufMetadata m;
    m.block_count = 32;
    m.key_length = 128;
    m.head_count_kv_per_layer.assign(32, 4);

    derive_scalars(m);
    // Expected: 128 total heads * 128 key_len * 2[F16] * 2[K+V] = 65536
    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);

    check("standard: kv_bytes = total_heads * key_len * 4",
          approx_eq(bytes, 128.0 * 128.0 * 4.0));
}

static void test_swa_precise() {
    GgufMetadata m;
    m.block_count = 4;
    m.key_length = 256;
    m.key_length_swa = 128;  // SWA layers use half the key dim

    // 2 full layers (8 heads each), 2 SWA layers (4 heads each)
    m.head_count_kv_per_layer = {8, 4, 8, 4};
    m.sliding_window_pattern = {false, true, false, true};

    derive_scalars(m);

    // Expected weighted sum:
    //   layer 0: 8 * 256 = 2048  (full)
    //   layer 1: 4 * 128 = 512   (swa)
    //   layer 2: 8 * 256 = 2048  (full)
    //   layer 3: 4 * 128 = 512   (swa)
    //   total weighted = 5120
    //   bytes = 5120 * 4 = 20480
    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);

    check("swa-precise: weighted kv_bytes correct",
          approx_eq(bytes, 5120.0 * 4.0));

    double scale = 0;
    lemon::compute_weighted_kv_cache_bytes_per_token(m, &scale);
    // weighted = 8*256 + 4*128 + 8*256 + 4*128 = 5120
    // unweighted = (8+4+8+4) * 256 = 6144
    // scale = 5120 / 6144 ≈ 0.8333
    check("swa-precise: scale factor < 1.0",
          scale > 0.0 && scale < 1.0);
    check("swa-precise: scale ≈ 0.8333",
          approx_eq(scale, 5120.0 / 6144.0));
}

static void test_swa_scalar_fallback() {
    GgufMetadata m;
    m.block_count = 4;
    m.key_length = 256;
    m.key_length_swa = 128;

    // Scalar case: no per-layer array, no sliding_window_pattern
    m.head_count_kv_scalar = 8;

    derive_scalars(m);
    // After derivation, per_layer IS populated from scalar, and swa_pattern is empty.
    // Since swa_pattern is empty, the precise path is NOT taken.
    // Falls back to proportional: factor = 1 - swa_ratio + swa_ratio * dim_ratio
    // But swa_layer_count = 0 (no pattern), so factor = 1.0

    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);
    // No SWA detected (no pattern), so standard formula:
    // 32 heads * 256 * 4 = 32768
    check("swa-scalar-no-pattern: uses standard formula when no pattern",
          approx_eq(bytes, 32.0 * 256.0 * 4.0));
}

static void test_swa_scalar_with_count() {
    GgufMetadata m;
    m.block_count = 4;
    m.key_length = 256;
    m.key_length_swa = 128;
    m.swa_layer_count = 2;

    m.head_count_kv_scalar = 8;

    derive_scalars(m);
    // per_layer populated from scalar, pattern empty → proportional fallback
    // factor = 1 - 2/4 + 2/4 * 128/256 = 1 - 0.5 + 0.25 = 0.75
    // bytes = 32 * 256 * 4 * 0.75 = 24576

    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);
    check("swa-scalar-count: proportional factor applied",
          approx_eq(bytes, 32.0 * 256.0 * 4.0 * 0.75));
}

static void test_full_attention_interval() {
    // For each (blocks, interval), verify the exact count:
    // floor((blocks - 1) / interval) + 1
    struct TestCase {
        int64_t blocks;
        int64_t interval;
        int64_t expected_full_layers;
        double expected_factor;  // expected_full_layers / blocks
    };

    TestCase cases[] = {
        {1, 12, 1, 1.0 / 1},        // single layer, all attention
        {11, 12, 1, 1.0 / 11},      // less than one interval
        {12, 12, 1, 1.0 / 12},      // exactly one interval (layer 0 only)
        {13, 12, 2, 2.0 / 13},      // just over one interval
        {36, 12, 3, 3.0 / 36},      // layers 0, 12, 24
        {37, 12, 4, 4.0 / 37},      // layers 0, 12, 24, 36
        {48, 12, 4, 4.0 / 48},      // layers 0, 12, 24, 36
        {49, 12, 5, 5.0 / 49},      // layers 0, 12, 24, 36, 48
        {96, 12, 8, 8.0 / 96},      // evenly divisible
        {97, 12, 9, 9.0 / 97},      // one over
    };

    for (const auto& tc : cases) {
        GgufMetadata m;
        m.block_count = tc.blocks;
        m.key_length = 128;
        m.full_attention_interval = tc.interval;
        m.head_count_kv_per_layer.assign(tc.blocks, 4);

        derive_scalars(m);

        double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);
        double scale = 0;
        lemon::compute_weighted_kv_cache_bytes_per_token(m, &scale);

        // Total heads = blocks * 4, base bytes = total_heads * 128 * 4
        double base_bytes = static_cast<double>(tc.blocks) * 4.0 * 128.0 * 4.0;
        double expected_bytes = base_bytes * tc.expected_factor;

        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "fai: blocks=%ld interval=%ld → factor=%.4f",
                      (long)tc.blocks, (long)tc.interval, scale);
        check(buf, approx_eq(scale, tc.expected_factor, 0.001));

        std::snprintf(buf, sizeof(buf),
                      "fai: blocks=%ld interval=%ld → bytes",
                      (long)tc.blocks, (long)tc.interval);
        check(buf, approx_eq(bytes, expected_bytes, 1.0));
    }
}

static void test_fai_improvement() {
    // Demonstrate that the exact formula differs meaningfully from 1/interval
    // for non-divisible block counts.
    GgufMetadata m;
    m.block_count = 37;
    m.key_length = 128;
    m.full_attention_interval = 12;
    m.head_count_kv_per_layer.assign(37, 4);

    derive_scalars(m);

    double scale = 0;
    lemon::compute_weighted_kv_cache_bytes_per_token(m, &scale);

    double old_approx = 1.0 / 12.0;   // ≈ 0.0833
    double exact = 4.0 / 37.0;        // ≈ 0.1081

    check("fai-improvement: exact ≠ old approximation for 37/12",
          !approx_eq(old_approx, exact, 0.01));
    check("fai-improvement: uses exact count (4/37 ≈ 0.1081)",
          approx_eq(scale, exact, 0.001));
    check("fai-improvement: exact > old (conservative: over-estimates KV)",
          scale > old_approx);
}

static void test_missing_metadata() {
    GgufMetadata m_empty;
    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m_empty);
    check("missing: no metadata returns 0", bytes == 0.0);

    GgufMetadata m_no_blocks;
    m_no_blocks.key_length = 128;
    m_no_blocks.head_count_kv = 64;
    bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m_no_blocks);
    check("missing: no block_count returns 0", bytes == 0.0);

    GgufMetadata m_no_keylen;
    m_no_keylen.block_count = 32;
    m_no_keylen.head_count_kv = 64;
    bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m_no_keylen);
    check("missing: no key_length returns 0", bytes == 0.0);
}

static void test_varying_heads_swa() {
    // Model where SWA layers have FEWER heads than full layers.
    GgufMetadata m;
    m.block_count = 6;
    m.key_length = 256;
    m.key_length_swa = 64;  // 4x reduction in SWA

    // Full layers: 16 heads, SWA layers: 4 heads
    m.head_count_kv_per_layer = {16, 4, 16, 4, 16, 4};
    m.sliding_window_pattern = {false, true, false, true, false, true};

    derive_scalars(m);

    // Precise:
    //   full: 16*256 + 16*256 + 16*256 = 12288
    //   swa:  4*64  + 4*64  + 4*64  = 768
    //   weighted = 13056
    //   bytes = 13056 * 4 = 52224
    double bytes = lemon::compute_weighted_kv_cache_bytes_per_token(m);
    check("varying-heads-swa: precise weighted sum",
          approx_eq(bytes, 13056.0 * 4.0));

    // Old proportional approximation (with uniform head count = total/6 = 6):
    //   factor = 1 - 3/6 + 3/6 * 64/256 = 1 - 0.5 + 0.125 = 0.625
    //   bytes = 60 * 256 * 4 * 0.625 = 38400
    double old_approx = 60.0 * 256.0 * 4.0 * 0.625;
    check("varying-heads-swa: precise differs from proportional",
          !approx_eq(bytes, old_approx, 1000.0));
}



static void test_gemma4_bounded_swa() {
    GgufMetadata m;
    m.architecture = "gemma4";
    m.block_count = 48;
    m.key_length = 512;
    m.value_length = 512;
    m.key_length_swa = 256;
    m.value_length_swa = 256;
    m.sliding_window = 1024;
    for (int i = 0; i < 48; ++i) {
        const bool sliding = i % 6 != 5;
        m.sliding_window_pattern.push_back(sliding);
        m.head_count_kv_per_layer.push_back(sliding ? 8 : 1);
    }
    derive_scalars(m);

    const double unbounded = lemon::compute_weighted_kv_cache_bytes_per_token(m);
    const auto profile = lemon::compute_bounded_swa_profile(m, "llamacpp");
    check("Gemma4 12B: original 40-SWA / 8-full classification preserved",
          approx_eq(unbounded, 344064.0) && profile &&
          approx_eq(profile->swa_bytes_per_cell, 327680.0));
    check("Gemma4 12B: conservative non-FA V padding is included",
          profile && approx_eq(profile->full_bytes_per_token, 40960.0));

    constexpr double draft_bytes_per_token = 26624.0;
    const double old_slope = unbounded * lemon::AUTO_CTX_MTP_KV_MULTIPLIER +
                             draft_bytes_per_token;
    const auto before = lemon::estimate_auto_context_budget(
        31.8, 7.75, old_slope, true, true);
    check("Gemma4 12B MTP: reproduces observed 60k limit",
          before.max_tokens >= 58000 && before.max_tokens < 62000);
    if (profile) {
        const double safe_slope = profile->unbounded_bytes_per_token() *
                                      lemon::AUTO_CTX_MTP_KV_MULTIPLIER +
                                  draft_bytes_per_token;
        const auto budget = lemon::estimate_auto_context_budget(
            31.8, 7.75, safe_slope, true, true);
        const auto ctx = lemon::bounded_swa_context_limit(
            *profile, budget.kv_gib,
            profile->full_bytes_per_token * lemon::AUTO_CTX_MTP_KV_MULTIPLIER +
                draft_bytes_per_token,
            profile->swa_bytes_per_cell * lemon::AUTO_CTX_MTP_KV_MULTIPLIER,
            262144);
        check("Gemma4 12B MTP: full 262144-token context fits 5090 budget",
              ctx == 262144);
        const auto hrx_profile = lemon::compute_bounded_swa_profile(m, "llamacpp-hrx");
        check("Gemma4 HRX uses the same SWA estimate",
              hrx_profile &&
              approx_eq(hrx_profile->full_bytes_per_token, profile->full_bytes_per_token) &&
              approx_eq(hrx_profile->swa_bytes_per_cell, profile->swa_bytes_per_cell));
    }
    m.architecture = "qwen35";
    check("Qwen hybrid: no ISWA optimization",
          !lemon::compute_bounded_swa_profile(m, "llamacpp"));
    m.architecture = "gemma4";
    m.sliding_window = 0;
    check("Gemma4 without window: conservative estimate",
          !lemon::compute_bounded_swa_profile(m, "llamacpp"));
    m.sliding_window = 1024;
    m.sliding_window_pattern.pop_back();
    check("Gemma4 with incomplete per-layer pattern: conservative estimate",
          !lemon::compute_bounded_swa_profile(m, "llamacpp"));
}

static void test_auto_context_budget() {
    using lemon::estimate_auto_context_budget;
    // 5090 (32 GiB), Qwen3.8-27B Q4 (16.5 GiB), vision projector and MTP.
    auto qwen = estimate_auto_context_budget(32.0, 18.0, 65536.0 * 1.07,
                                             true, true);
    check("auto ctx: CUDA uses most VRAM without exhausting it",
          qwen.max_tokens >= 170000 && qwen.max_tokens < 215000);
    check("auto ctx: dGPU runtime reserve is bounded",
          qwen.reserve_gib >= 1.5 && qwen.reserve_gib <= 3.0);
    check("auto ctx: GGUF weights counted once",
          approx_eq(qwen.model_gib, 18.0));
    using lemon::auto_context_total_weights_gib;
    check("auto ctx: model registry already includes mmproj",
          approx_eq(auto_context_total_weights_gib(18.0, 16.5, 1.5, 0.0), 18.0));
    check("auto ctx: incomplete registry gets resolved extras",
          approx_eq(auto_context_total_weights_gib(16.5, 16.5, 1.5, 0.0), 18.0));
    check("auto ctx: sharded total is not replaced by first shard",
          approx_eq(auto_context_total_weights_gib(21.0, 1.1, 0.0, 0.0), 21.0));

    auto no_extra = estimate_auto_context_budget(32.0, 16.5, 65536.0,
                                                 true, false);
    check("auto ctx: mmproj and MTP reduce available context",
          qwen.max_tokens < no_extra.max_tokens);

    auto apu = estimate_auto_context_budget(110.0, 18.0, 65536.0,
                                            true, false, true);
    check("auto ctx: 128 GB APU still supports long context",
          apu.max_tokens >= 262144);
    check("auto ctx: UMA retains OS memory headroom",
          apu.reserve_gib >= 8.0 && apu.reserve_gib < 14.0);

    auto discrete = estimate_auto_context_budget(110.0, 18.0, 65536.0,
                                                 true, false, false);
    check("auto ctx: discrete and UMA budgets differ",
          discrete.reserve_gib < apu.reserve_gib);
    auto cpu_budget = estimate_auto_context_budget(16.0, 5.0, 65536.0,
                                                    false, false);
    auto cpu_with_uma_hint = estimate_auto_context_budget(16.0, 5.0, 65536.0,
                                                           false, false, true, 24.0);
    check("auto ctx: CPU budget ignores any UMA GPU hint",
          cpu_budget.max_tokens == cpu_with_uma_hint.max_tokens);
    check("auto ctx: host workspace minimum is 0.5 GiB",
          approx_eq(cpu_budget.reserve_gib, 1.5));
    check("auto ctx: dGPU headroom is fixed at 1 GiB",
          approx_eq(lemon::AUTO_CTX_DISCRETE_GPU_RESERVE.memory_headroom.estimate(8.0), 1.0) &&
          approx_eq(lemon::AUTO_CTX_DISCRETE_GPU_RESERVE.memory_headroom.estimate(32.0), 1.0));
    auto original_apple = estimate_auto_context_budget(3.2, 0.73, 128.0 * 1024.0,
                                                      true, false, true);
    auto apple_no_physical_reading = estimate_auto_context_budget(
        3.2, 0.73, 128.0 * 1024.0, true, false, true, -1.0);
    check("auto ctx: Apple retains existing budget without AMD split",
          original_apple.max_tokens == apple_no_physical_reading.max_tokens);

    // A 128 GiB Strix Halo has ~64 GiB GTT by default, but far more
    // physically free RAM. OS headroom must not consume GTT capacity.
    auto strix64 = estimate_auto_context_budget(64.0, 58.5, 65536.0,
                                                true, false, true, 116.0);
    check("auto ctx: Strix Halo 64 GiB GTT can load gpt-oss-120b",
          strix64.max_tokens >= 8192 && strix64.kv_gib > 3.0);
    auto strix120 = estimate_auto_context_budget(120.0, 58.5, 65536.0,
                                                 true, false, true, 116.0);
    check("auto ctx: 120 GiB GTT honors RAM headroom",
          strix120.kv_gib < 47.0 && strix120.kv_gib > 45.0);
    auto ram_pressure = estimate_auto_context_budget(64.0, 58.5, 65536.0,
                                                     true, false, true, 62.0);
    check("auto ctx: UMA rejects true physical RAM exhaustion",
          ram_pressure.max_tokens == 0);
    auto gtt_pressure = estimate_auto_context_budget(60.0, 58.5, 65536.0,
                                                     true, false, true, 116.0);
    check("auto ctx: UMA GPU-only budget retains workspace reserve",
          gtt_pressure.max_tokens == 0);
    using lemon::auto_context_cpu_offload_fallback;
    // GPT-OSS MXFP4 exceeds the default GTT once the GPU workspace is included.
    auto mxfp4_gpu = estimate_auto_context_budget(64.0, 63.4, 65536.0,
                                                  true, false, true, 116.0);
    check("auto ctx: default GTT cannot fully host 63.4 GiB weights",
          mxfp4_gpu.max_tokens == 0);
    check("auto ctx: UMA permits CPU spill with a small context",
          auto_context_cpu_offload_fallback(
              mxfp4_gpu.max_tokens, 64.0, 116.0, 63.4, 65536.0,
              false, true, true) == 4096);
    check("auto ctx: UMA can use CPU when GTT pool is fully occupied",
          auto_context_cpu_offload_fallback(
              0, 0.0, 116.0, 63.4, 65536.0,
              false, true, true) == 4096);
    check("auto ctx: GPU-first policy does not spill when GPU fits",
          auto_context_cpu_offload_fallback(
              strix64.max_tokens, 64.0, 116.0, 58.5, 65536.0,
              false, true, true) == 0);
    check("auto ctx: positive but too-small GPU context tries CPU offload",
          auto_context_cpu_offload_fallback(819, 8.0, 48.0, 6.7,
                                            65536.0, false, false, true) == 4096);
    check("auto ctx: GPU context at minimum keeps GPU-first placement",
          auto_context_cpu_offload_fallback(4096, 8.0, 48.0, 6.7,
                                            65536.0, false, false, true) == 0);
    check("auto ctx: dGPU may spill when VRAM does not fit model",
          auto_context_cpu_offload_fallback(
              0, 22.0, 48.0, 60.0, 65536.0, false, false, true) == 4096);
    check("auto ctx: dGPU may spill even with no VRAM left",
          auto_context_cpu_offload_fallback(
              0, 0.0, 68.0, 60.0, 65536.0, false, false, true) == 4096);
    check("auto ctx: borderline dGPU split respects both reserves",
          auto_context_cpu_offload_fallback(
              0, 24.0, 36.5, 60.0, 65536.0, false, false, true) == 0);
    check("auto ctx: dGPU split fits after both reserves",
          auto_context_cpu_offload_fallback(
              0, 24.0, 44.0, 60.0, 65536.0, false, false, true) == 4096);
    check("auto ctx: host-only spill rejects missing runtime headroom",
          auto_context_cpu_offload_fallback(
              0, 0.0, 4.0, 3.6, 65536.0, false, false, true) == 0);
    check("auto ctx: dGPU cannot use unavailable combined RAM/VRAM",
          auto_context_cpu_offload_fallback(
              0, 8.0, 12.0, 60.0, 65536.0, false, false, true) == 0);
    check("auto ctx: unmeasured dGPU RAM lets llama.cpp try",
          auto_context_cpu_offload_fallback(
              0, 8.0, -1.0, 60.0, 65536.0, false, false, true) == 4096);
    check("auto ctx: non-llama backends retain zero-capacity failure",
          auto_context_cpu_offload_fallback(
              0, 22.0, 48.0, 60.0, 65536.0, false, false, false) == 0);
    check("auto ctx: UMA rejects CPU spill under physical RAM pressure",
          auto_context_cpu_offload_fallback(
              0, 64.0, 67.0, 63.4, 65536.0, false, true, true) == 0);
    check("auto ctx: UMA embeds require sufficient physical RAM for 8192 tokens",
          auto_context_cpu_offload_fallback(
              0, 64.0, 116.0, 63.4, 65536.0,
              false, true, true, 8192) == 8192);

    auto small_mac = estimate_auto_context_budget(3.2, 0.73, 128.0 * 1024.0,
                                                   true, false, true);
    check("auto ctx: 7 GiB Mac with 3.2 GiB free can load a small model",
          small_mac.max_tokens >= 8192);
    check("auto ctx: small UMA pool retains runtime headroom",
          small_mac.reserve_gib >= 1.0 && small_mac.reserve_gib < 2.0);
    auto constrained_mac = estimate_auto_context_budget(1.3, 0.73, 128.0 * 1024.0,
                                                         true, false, true);
    check("auto ctx: small UMA pool still rejects genuine memory exhaustion",
          constrained_mac.max_tokens == 0);
    auto larger_uma = estimate_auto_context_budget(24.0, 0.73, 128.0 * 1024.0,
                                                   true, false, true);
    check("auto ctx: larger UMA pool retains proportional headroom",
          larger_uma.reserve_gib > 2.0);

    auto small = estimate_auto_context_budget(8.0, 3.0, 65536.0,
                                              true, false);
    check("auto ctx: 8 GiB GPU has compute headroom",
          small.reserve_gib >= 1.0 && small.reserve_gib <= 2.5);
    check("auto ctx: 8 GiB GPU provides useful context",
          small.max_tokens >= 32768);

    auto no_space = estimate_auto_context_budget(4.0, 5.0, 65536.0,
                                                 true, false);
    check("auto ctx: weights exceeding VRAM cannot claim KV", no_space.max_tokens == 0);
    auto missing_model = estimate_auto_context_budget(32.0, 0.0, 65536.0,
                                                      true, false);
    check("auto ctx: missing model size handled", missing_model.max_tokens == 0);
    auto invalid_kv = estimate_auto_context_budget(32.0, 16.5, 0.0,
                                                   true, false);
    check("auto ctx: invalid KV metadata handled", invalid_kv.max_tokens == 0);

    auto barely_fits = estimate_auto_context_budget(8.0, 7.875, 65536.0,
                                                     true, false);
    check("auto ctx: known insufficient VRAM returns zero, not 4096",
          barely_fits.max_tokens == 0);
    auto tiny_budget = estimate_auto_context_budget(8.0, 6.7, 65536.0,
                                                     true, false);
    check("auto ctx: low but positive budget is not raised to a fallback",
          tiny_budget.max_tokens > 0 && tiny_budget.max_tokens < 4096);

    using lemon::auto_context_clamp_tokens;
    auto embedding_budget = estimate_auto_context_budget(41.4, 0.51,
                                                          128.0 * 1024.0,
                                                          true, false, true);
    check("auto ctx: embedding budget can hold 8192 tokens on 41.4 GiB UMA",
          embedding_budget.max_tokens > 8192);
    check("auto ctx: model maximum smaller than runtime floor is not OOM",
          auto_context_clamp_tokens(embedding_budget.max_tokens, 512, 32768, 8192) == 8192);
    check("auto ctx: runtime minimum requires actual capacity",
          auto_context_clamp_tokens(4096, 512, 32768, 8192) == 0);
    check("auto ctx: model maximum is honored for normal LLMs",
          auto_context_clamp_tokens(100000, 512, 32768) == 512);
    check("auto ctx: unknown maximum has a bounded default",
          auto_context_clamp_tokens(100000, 0, 32768) == 32768);
    check("auto ctx: model maximum above runtime minimum remains capped",
          auto_context_clamp_tokens(100000, 16384, 32768, 8192) == 16384);

    GgufMetadata bert_encoder;
    bert_encoder.architecture = "nomic-bert-moe";
    bert_encoder.block_count = 12;
    bert_encoder.embedding_length = 768;
    const double encoder_kv = lemon::estimate_bert_encoder_kv_bytes_per_token(
        bert_encoder, true);
    check("auto ctx: BERT encoder estimates full-width K/V from GGUF",
          approx_eq(encoder_kv, 12.0 * 768.0 * 4.0));
    check("auto ctx: non-encoder retains the existing KV fallback",
          lemon::estimate_bert_encoder_kv_bytes_per_token(bert_encoder, false) == 0.0);
    bert_encoder.embedding_length = 0;
    check("auto ctx: missing encoder dimensions retain the size fallback",
          lemon::estimate_bert_encoder_kv_bytes_per_token(bert_encoder, true) == 0.0);

    // Values observed on the 7 GiB macOS CI runner for nomic-embed-text-v2-moe.
    auto generic_embedding = estimate_auto_context_budget(2.40, 0.48,
                                                          128.0 * 1024.0,
                                                          true, false, true);
    check("auto ctx: generic fallback rejects the observed macOS sample",
          generic_embedding.max_tokens < 8192);
    auto mac_embedding = estimate_auto_context_budget(2.40, 0.48, encoder_kv,
                                                      true, false, true);
    check("auto ctx: measured macOS embedding budget fits 8192 tokens",
          auto_context_clamp_tokens(mac_embedding.max_tokens, 512, 32768, 8192) == 8192);
    check("auto ctx: measured macOS runtime reserve stays intact",
          approx_eq(mac_embedding.reserve_gib, 1.25));
    auto mac_insufficient = estimate_auto_context_budget(1.90, 0.48, encoder_kv,
                                                         true, false, true);
    check("auto ctx: real macOS embedding shortage is still rejected",
          auto_context_clamp_tokens(mac_insufficient.max_tokens, 512, 32768, 8192) == 0);

    using lemon::auto_context_cap_shared_memory_gib;
    check("auto ctx: unified RAM cap uses physical RAM total",
          approx_eq(auto_context_cap_shared_memory_gib(20.0, 32.0, 24.0), 8.0));
    check("auto ctx: unified RAM cap leaves tighter GPU limit intact",
          approx_eq(auto_context_cap_shared_memory_gib(12.0, 32.0, 4.0), 12.0));
    check("auto ctx: unknown RAM usage does not fabricate a cap",
          approx_eq(auto_context_cap_shared_memory_gib(12.0, 32.0, -1.0), 12.0));
    check("auto ctx: default GTT does not subtract physical RAM usage from GPU limit",
          approx_eq(auto_context_cap_shared_memory_gib(64.0, 124.0, 12.0), 64.0));

    using lemon::auto_context_available_gpu_gib;
    auto cuda_free = auto_context_available_gpu_gib(32.0, 4.0, false, 0.0, -1.0);
    check("auto ctx: measured discrete VRAM remains unchanged",
          cuda_free && approx_eq(*cuda_free, 28.0));
    check("auto ctx: unknown discrete VRAM is unavailable",
          !auto_context_available_gpu_gib(32.0, -1.0, false, 0.0, 12.0));

    auto metal_free = auto_context_available_gpu_gib(24.0, -1.0, true, 32.0, 24.0);
    check("auto ctx: unified GPU uses measured physical RAM without VRAM metrics",
          metal_free && approx_eq(*metal_free, 8.0));
    check("auto ctx: unknown unified VRAM and RAM use safe fallback",
          !auto_context_available_gpu_gib(24.0, -1.0, true, 32.0, 0.0));
    check("auto ctx: invalid unified RAM and GPU readings use safe fallback",
          !auto_context_available_gpu_gib(24.0, -1.0, true, 32.0, -1.0));

    auto shared_free = auto_context_available_gpu_gib(24.0, 2.0, true, 32.0, 25.0);
    check("auto ctx: shared GPU applies tighter RAM limit",
          shared_free && approx_eq(*shared_free, 7.0));
    check("auto ctx: GPU usage alone cannot establish shared RAM availability",
          !auto_context_available_gpu_gib(24.0, 2.0, true, 32.0, 0.0));
    check("auto ctx: unified GPU requires known physical RAM capacity",
          !auto_context_available_gpu_gib(24.0, 2.0, true, 0.0, 20.0));
    check("auto ctx: unified GPU rejects nonfinite RAM usage",
          !auto_context_available_gpu_gib(24.0, 2.0, true, 32.0,
                                          std::numeric_limits<double>::quiet_NaN()));

    auto huge = estimate_auto_context_budget(110.0, 18.0, 1e-12,
                                              true, false);
    check("auto ctx: token conversion saturates without int64 overflow",
          huge.max_tokens == std::numeric_limits<int64_t>::max());
}

static void set_swa_test_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static GgufMetadata make_swa_fixture(const char* architecture, int64_t n_layers = 24) {
    GgufMetadata m;
    m.architecture = architecture;
    m.block_count = n_layers;
    m.embedding_length = 1024;
    m.head_count = 8;
    m.key_length = 128;
    m.value_length = 128;
    m.sliding_window = 1024;
    m.head_count_kv_per_layer.assign(static_cast<size_t>(n_layers), 4);
    m.head_count_kv = n_layers * 4;
    return m;
}

static void test_bounded_swa_architectures() {
    using lemon::compute_bounded_swa_profile;
    auto m = make_swa_fixture("gemma2", 4);
    auto p = compute_bounded_swa_profile(m, "llamacpp");
    check("Gemma2: implicit 2-period with SWA first",
          p && p->full_bytes_per_token == p->swa_bytes_per_cell);

    m = make_swa_fixture("gemma3", 6);
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("Gemma3: 5 SWA / 1 full despite equal K/V dimensions",
          p && approx_eq(p->swa_bytes_per_cell, 5.0 * p->full_bytes_per_token));

    m = make_swa_fixture("gpt-oss", 24);
    check("GPT-OSS: 12 SWA / 12 full ISWA layout",
          compute_bounded_swa_profile(m, "llamacpp").has_value());
    m = make_swa_fixture("cohere2", 8);
    check("Cohere2: periodic ISWA layout",
          compute_bounded_swa_profile(m, "llamacpp").has_value());
    m = make_swa_fixture("cohere2moe", 4);
    m.head_count_kv_per_layer = {1, 8, 8, 8};
    m.head_count_kv = 25;
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("Cohere2 MoE: dense-first phase",
          p && p->swa_bytes_per_cell > p->full_bytes_per_token);
    m = make_swa_fixture("olmo2", 32);
    check("OLMo2/3: optional SWA layout",
          compute_bounded_swa_profile(m, "llamacpp").has_value());
    m = make_swa_fixture("exaone4", 64);
    check("EXAONE4 32B: ISWA layout",
          compute_bounded_swa_profile(m, "llamacpp").has_value());
    m.block_count = 30;
    m.head_count_kv_per_layer.resize(30);
    m.head_count_kv = 120;
    check("EXAONE4 1.2B: no bounded cache",
          !compute_bounded_swa_profile(m, "llamacpp"));

    m = make_swa_fixture("gemma3", 6);
    m.sliding_window_pattern_scalar_present = true;
    m.sliding_window_pattern_period = 3;
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("scalar GGUF period overrides architecture default",
          p && approx_eq(p->swa_bytes_per_cell, 2.0 * p->full_bytes_per_token));
    m.sliding_window_pattern = {false, true, true, true, true, true};
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("explicit array overrides scalar period and default phase",
          p && approx_eq(p->swa_bytes_per_cell, 5.0 * p->full_bytes_per_token));

    m = make_swa_fixture("gemma4", 4);
    m.sliding_window_pattern.assign(4, true);
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("pure SWA: exactly zero full-attention slope",
          p && p->full_bytes_per_token == 0.0);
    check("pure SWA: full context when fixed cache fits",
          p && lemon::bounded_swa_context_limit(*p, 1.0, 0.0,
                  p->swa_bytes_per_cell, 262144) == 262144);

    m = make_swa_fixture("gemma4", 4);
    m.sliding_window_pattern = {false, true, false, true};
    m.head_count_kv_per_layer = {1, 8, 4, 2};
    m.head_count_kv = 15;
    m.key_length = 64;
    m.value_length = 128;
    m.key_length_swa = 16;
    m.value_length_swa = 32;
    p = compute_bounded_swa_profile(m, "llamacpp");
    check("asymmetric K/V and per-layer KV heads, padded V allocation",
          p && approx_eq(p->full_bytes_per_token, 2688.0) &&
          approx_eq(p->swa_bytes_per_cell, 2368.0));
}

static void test_bounded_swa_fallbacks_and_runtime() {
    // Host LLAMA_ARG_* settings must not affect positive argument checks.
    const std::vector<const char*> keys = {
        "LLAMA_ARG_N_PARALLEL", "LLAMA_ARG_CACHE_TYPE_V", "LLAMA_ARG_CACHE_TYPE_K",
        "LLAMA_ARG_UBATCH", "LLAMA_ARG_SWA_FULL", "LLAMA_ARG_KV_UNIFIED"};
    std::vector<std::pair<std::string, std::optional<std::string>>> previous;
    for (const char* key : keys) {
        const char* value = std::getenv(key);
        previous.push_back({key, value ? std::optional<std::string>(value) : std::nullopt});
        set_swa_test_env(key, nullptr);
    }
    using lemon::compute_bounded_swa_profile;
    auto m = make_swa_fixture("gemma4", 4);
    check("Gemma4 missing explicit pattern: conservative fallback",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.sliding_window_pattern = {true, false};
    check("incomplete array: conservative fallback",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.sliding_window_pattern = {true, false, true, false};
    m.sliding_window_pattern_valid = false;
    check("invalid array: conservative fallback",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.sliding_window_pattern_valid = true;
    m.shared_kv_layers = 2;
    check("virtual/reused KV layers: conservative fallback",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.shared_kv_layers = 0;
    m.value_length = 0;
    m.embedding_length = 0;
    check("unknown V dimension: do not assume V equals K",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m = make_swa_fixture("gemma3", 6);
    m.sliding_window = 0;
    check("missing optional Gemma3 window: conservative fallback",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m = make_swa_fixture("gemma2", 4);
    m.sliding_window = 0;
    check("Gemma2 verified 4096 window default",
          compute_bounded_swa_profile(m, "llamacpp") &&
          compute_bounded_swa_profile(m, "llamacpp")->window == 4096);
    m.full_attention_interval = 2;
    check("hybrid recurrent model: never treat SSM state as KV",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m = make_swa_fixture("phi3", 4);
    m.sliding_window_pattern = {true, false, true, false};
    check("Phi3's sliding-window metadata does not imply ISWA",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.architecture = "llama4";
    check("Llama4 chunked attention is not ISWA",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.architecture = "qwen35";
    check("hybrid Qwen SSM is excluded",
          !compute_bounded_swa_profile(m, "llamacpp"));
    m.architecture = "granite_swa";
    check("granite-swa is upstream-only until HRX loader supports it",
          compute_bounded_swa_profile(m, "llamacpp") &&
          !compute_bounded_swa_profile(m, "llamacpp-hrx"));

    using lemon::bounded_swa_runtime_compatible;
    check("parallel 1 and -np 1 preserve bounded SWA",
          bounded_swa_runtime_compatible("--parallel 1 -np=1") &&
          bounded_swa_runtime_compatible("-np 1 --parallel=1"));
    check("non-default --parallel is rejected",
          !bounded_swa_runtime_compatible("--parallel 1 --parallel=2"));
    check("-np 2 is rejected",
          !bounded_swa_runtime_compatible("-np 2"));
    check("--swa-full rejects optimization",
          !bounded_swa_runtime_compatible("--swa-full"));
    check("custom microbatch rejects optimization",
          !bounded_swa_runtime_compatible("--ubatch-size=8192") &&
          !bounded_swa_runtime_compatible("-ub 1024"));
    check("K/V quantized cache rejects optimization",
          !bounded_swa_runtime_compatible("-ctk=q8_0") &&
          !bounded_swa_runtime_compatible("--cache-type-v q4_0"));
    check("KV unified overrides reject optimization",
          !bounded_swa_runtime_compatible("--kv-unified"));
    check("flag substrings are not mistaken for a cache flag",
          bounded_swa_runtime_compatible("--parallelism-other 2"));
    check("HRX reads hrx_args, not llamacpp_args",
          !lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp-hrx", "--parallel 2", "--swa-full") &&
          lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp-hrx", "--parallel 2", "--parallel 1") &&
          !lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "--swa-full", "--parallel 1"));
    check("merge_args=false needs an explicit single slot",
          !lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "", "", false) &&
          lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "-np 1", "", false) &&
          !lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "-np 2", "", false));
    check("unknown system llama.cpp disables bounded SWA",
          !lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "--parallel 1", "", true, "system") &&
          lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp", "--parallel 1", "", true, "rocm") &&
          lemon::bounded_swa_runtime_compatible_for_recipe(
              "llamacpp-hrx", "", "", true, "system"));
    check("both recipe types use one SWA profile",
          compute_bounded_swa_profile(make_swa_fixture("gemma3", 6), "llamacpp") &&
          compute_bounded_swa_profile(make_swa_fixture("gemma3", 6), "llamacpp-hrx"));

    // Test environment overrides after the environment-independent checks.
    check("LLAMA_ARG_N_PARALLEL=unset is safe", bounded_swa_runtime_compatible(""));
    set_swa_test_env("LLAMA_ARG_N_PARALLEL", "1");
    check("LLAMA_ARG_N_PARALLEL=1 is safe", bounded_swa_runtime_compatible(""));
    check("env pins one slot even with merge_args=false",
          bounded_swa_runtime_compatible("", false));
    set_swa_test_env("LLAMA_ARG_N_PARALLEL", "2");
    check("LLAMA_ARG_N_PARALLEL=2 forces full-KV fallback",
          !bounded_swa_runtime_compatible(""));
    set_swa_test_env("LLAMA_ARG_N_PARALLEL", nullptr);
    set_swa_test_env("LLAMA_ARG_CACHE_TYPE_V", "q8_0");
    check("LLAMA_ARG_CACHE_TYPE_V override forces fallback",
          !bounded_swa_runtime_compatible(""));
    set_swa_test_env("LLAMA_ARG_CACHE_TYPE_V", nullptr);
    set_swa_test_env("LLAMA_ARG_SWA_FULL", "1");
    check("LLAMA_ARG_SWA_FULL override forces fallback",
          !bounded_swa_runtime_compatible(""));
    set_swa_test_env("LLAMA_ARG_SWA_FULL", nullptr);
    set_swa_test_env("LLAMA_ARG_UBATCH", "8192");
    check("LLAMA_ARG_UBATCH override forces fallback",
          !bounded_swa_runtime_compatible(""));
    set_swa_test_env("LLAMA_ARG_UBATCH", nullptr);
    set_swa_test_env("LLAMA_ARG_KV_UNIFIED", "1");
    check("LLAMA_ARG_KV_UNIFIED override forces fallback",
          !bounded_swa_runtime_compatible(""));
    for (const auto& entry : previous)
        set_swa_test_env(entry.first.c_str(),
                         entry.second ? entry.second->c_str() : nullptr);
}

int main() {
    test_auto_context_budget();
    test_scalar_head_count_kv();
    test_array_head_count_kv();
    test_swa_pattern_derivation();
    test_standard_mha();
    test_swa_precise();
    test_swa_scalar_fallback();
    test_swa_scalar_with_count();
    test_full_attention_interval();
    test_fai_improvement();
    test_missing_metadata();
    test_varying_heads_swa();
    test_gemma4_bounded_swa();
    test_bounded_swa_architectures();
    test_bounded_swa_fallbacks_and_runtime();

    if (g_failures == 0) {
        std::printf("\nAll auto_tune tests passed\n");
        return 0;
    }
    std::printf("\n%d auto_tune test(s) FAILED\n", g_failures);
    return 1;
}
