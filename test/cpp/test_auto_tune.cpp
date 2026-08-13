// Standalone test for GGUF array storage and weighted KV cache computation.
//
// Compile: g++ -std=c++17 -I src/cpp/include test/cpp/test_auto_tune.cpp -o test_auto_tune

#include "lemon/gguf_reader.h"
#include <cmath>
#include <cstdio>
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

int main() {
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

    if (g_failures == 0) {
        std::printf("\nAll auto_tune tests passed\n");
        return 0;
    }
    std::printf("\n%d auto_tune test(s) FAILED\n", g_failures);
    return 1;
}
