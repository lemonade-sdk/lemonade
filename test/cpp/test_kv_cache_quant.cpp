// Standalone test for the KV-cache quant tier vocabulary, the GGUF
// value_length field it depends on, the safety table + eligibility gates,
// and the ladder resolver.
//
// Compile: g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include test/cpp/test_kv_cache_quant.cpp -o test_kv_cache_quant
// (The ladder resolver test cases link lemonade-server-core: resolve_kv_cache
// takes a RecipeOptions, which is not header-only.)

#include "lemon/auto_tune.h"
#include "lemon/backends/llamacpp/llamacpp.h"
#include "lemon/gguf_reader.h"
#include "lemon/kv_cache_quant.h"
#include <cmath>
#include <cstdio>
#include <string>

using lemon::GgufMetadata;
using lemon::KvCacheQuantConfig;
using lemon::KvCacheQuantTier;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool approx_eq(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

static void test_bytes_per_element_exact_values() {
    check("bytes_per_element: f16 == 2.0",
          approx_eq(lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::F16), 2.0));
    check("bytes_per_element: q8_0 == 1.0625",
          approx_eq(lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q8_0), 1.0625));
    check("bytes_per_element: q4_0 == 0.5625",
          approx_eq(lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q4_0), 0.5625));
}

static void test_bytes_per_element_not_round_numbers() {
    // Direct guard against a round-number regression:
    // 1.0/0.5 would under-reserve KV memory by 6.25%/12.5%.
    check("bytes_per_element: q8_0 is not 1.0",
          !approx_eq(lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q8_0), 1.0));
    check("bytes_per_element: q4_0 is not 0.5",
          !approx_eq(lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q4_0), 0.5));
}

static void test_block_size() {
    check("block_size: q8_0 == 32", lemon::kv_cache_quant_block_size(KvCacheQuantTier::Q8_0) == 32);
    check("block_size: q4_0 == 32", lemon::kv_cache_quant_block_size(KvCacheQuantTier::Q4_0) == 32);
    check("block_size: f16 == 1", lemon::kv_cache_quant_block_size(KvCacheQuantTier::F16) == 1);
}

static void test_config_parse_accepts_exact_set() {
    auto f16 = lemon::parse_kv_cache_quant_config("f16");
    auto auto_ = lemon::parse_kv_cache_quant_config("auto");
    auto q8 = lemon::parse_kv_cache_quant_config("q8_0");
    auto q4 = lemon::parse_kv_cache_quant_config("q4_0");
    check("config parse: f16 accepted", f16.has_value() && *f16 == KvCacheQuantConfig::F16);
    check("config parse: auto accepted", auto_.has_value() && *auto_ == KvCacheQuantConfig::Auto);
    check("config parse: q8_0 accepted", q8.has_value() && *q8 == KvCacheQuantConfig::Q8_0);
    check("config parse: q4_0 accepted", q4.has_value() && *q4 == KvCacheQuantConfig::Q4_0);
}

static void test_config_parse_rejects_out_of_set() {
    check("config parse: rejects q5_1 (wider llama.cpp set)",
          !lemon::parse_kv_cache_quant_config("q5_1").has_value());
    check("config parse: rejects wrong case",
          !lemon::parse_kv_cache_quant_config("Auto").has_value());
    check("config parse: rejects empty string",
          !lemon::parse_kv_cache_quant_config("").has_value());
}

static void test_round_trip_every_value() {
    for (auto v : {KvCacheQuantConfig::F16, KvCacheQuantConfig::Auto,
                   KvCacheQuantConfig::Q8_0, KvCacheQuantConfig::Q4_0}) {
        std::string s = lemon::kv_cache_quant_config_to_string(v);
        auto parsed = lemon::parse_kv_cache_quant_config(s);
        check(("config round-trip: " + s).c_str(), parsed.has_value() && *parsed == v);
    }
    for (auto v : {KvCacheQuantTier::F16, KvCacheQuantTier::Q8_0, KvCacheQuantTier::Q4_0}) {
        std::string s = lemon::kv_cache_quant_tier_to_string(v);
        auto parsed = lemon::parse_kv_cache_quant_tier(s);
        check(("tier round-trip: " + s).c_str(), parsed.has_value() && *parsed == v);
    }
}

static void test_narrowing_auto_is_not_a_concrete_tier() {
    auto narrowed = lemon::kv_cache_quant_tier_from_config(KvCacheQuantConfig::Auto);
    check("narrowing auto: reported as not-a-concrete-tier, not a default",
          !narrowed.has_value());
    auto f16 = lemon::kv_cache_quant_tier_from_config(KvCacheQuantConfig::F16);
    check("narrowing f16: resolves to KvCacheQuantTier::F16",
          f16.has_value() && *f16 == KvCacheQuantTier::F16);
    auto q8 = lemon::kv_cache_quant_tier_from_config(KvCacheQuantConfig::Q8_0);
    check("narrowing q8_0: resolves to KvCacheQuantTier::Q8_0",
          q8.has_value() && *q8 == KvCacheQuantTier::Q8_0);
    auto q4 = lemon::kv_cache_quant_tier_from_config(KvCacheQuantConfig::Q4_0);
    check("narrowing q4_0: resolves to KvCacheQuantTier::Q4_0",
          q4.has_value() && *q4 == KvCacheQuantTier::Q4_0);
}

static void test_ladder_order() {
    check("ladder: f16 higher quality than q8_0",
          lemon::kv_cache_quant_tier_higher_quality(KvCacheQuantTier::F16, KvCacheQuantTier::Q8_0));
    check("ladder: q8_0 higher quality than q4_0",
          lemon::kv_cache_quant_tier_higher_quality(KvCacheQuantTier::Q8_0, KvCacheQuantTier::Q4_0));
    check("ladder: f16 higher quality than q4_0",
          lemon::kv_cache_quant_tier_higher_quality(KvCacheQuantTier::F16, KvCacheQuantTier::Q4_0));
    check("ladder: q4_0 not higher quality than f16",
          !lemon::kv_cache_quant_tier_higher_quality(KvCacheQuantTier::Q4_0, KvCacheQuantTier::F16));
    check("ladder: rank strictly ascending f16 < q8_0 < q4_0",
          lemon::kv_cache_quant_tier_rank(KvCacheQuantTier::F16) <
              lemon::kv_cache_quant_tier_rank(KvCacheQuantTier::Q8_0) &&
          lemon::kv_cache_quant_tier_rank(KvCacheQuantTier::Q8_0) <
              lemon::kv_cache_quant_tier_rank(KvCacheQuantTier::Q4_0));
    // Ascending in context capacity: lower bytes-per-element -> more tokens fit.
    check("ladder: bytes_per_element strictly descending as rank ascends",
          lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::F16) >
              lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q8_0) &&
          lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q8_0) >
              lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q4_0));
}

static void test_gguf_value_length() {
    GgufMetadata with_value;
    with_value.value_length = 128;
    check("GgufMetadata: value_length exposed when present", with_value.value_length == 128);

    GgufMetadata without_value;
    check("GgufMetadata: value_length defaults to 0 when absent", without_value.value_length == 0);
}

static void test_weighted_byte_cost_uses_both_key_and_value_length() {
    // 32 uniform layers, 4 kv heads/layer -> head_count_kv=128 total, matching
    // the convention make_model() and every other GgufMetadata fixture in
    // this file already use for the scalar/uniform branch.
    GgufMetadata gguf;
    gguf.block_count = 32;
    gguf.head_count_kv = 128;
    gguf.key_length = 128;
    gguf.value_length = 64;  // asymmetric: half of key_length
    const double bytes_per_token = lemon::compute_weighted_kv_cache_bytes_per_token(gguf);
    const double expected = 128.0 * (128.0 + 64.0) * 2.0;  // f16: 2 bytes/element
    check("weighted byte cost sums key_length + value_length for an asymmetric model",
          approx_eq(bytes_per_token, expected));
    const double old_symmetric_assumption = 128.0 * 128.0 * 2.0 * 2.0;  // key_length doubled
    check("weighted byte cost differs from doubling key_length when key/value dims differ",
          !approx_eq(bytes_per_token, old_symmetric_assumption));
}

static void test_weighted_byte_cost_falls_back_value_length_to_key_length_when_absent() {
    GgufMetadata gguf;
    gguf.block_count = 32;
    gguf.head_count_kv = 128;
    gguf.key_length = 128;
    gguf.value_length = 0;  // absent from this model's GGUF metadata
    const double bytes_per_token = lemon::compute_weighted_kv_cache_bytes_per_token(gguf);
    const double expected = 128.0 * (128.0 + 128.0) * 2.0;
    check("weighted byte cost falls back value_length to key_length when the GGUF field is absent",
          approx_eq(bytes_per_token, expected));
}

static void test_safety_table_documented_answers() {
    using lemon::backends::llamacpp::kv_cache_quant_safety_table;
    struct Row { const char* backend; bool q8; bool q4; };
    const Row rows[] = {
        {"cuda", true, true},
        {"rocm-stable", true, true},
        {"rocm-nightly", true, true},
        {"metal", true, true},
        {"vulkan", false, false},
        {"cpu", false, false},
        {"system", false, false},
    };
    for (const auto& row : rows) {
        bool q8 = lemon::kv_cache_quant_backend_eligible(
            KvCacheQuantTier::Q8_0, row.backend, kv_cache_quant_safety_table);
        bool q4 = lemon::kv_cache_quant_backend_eligible(
            KvCacheQuantTier::Q4_0, row.backend, kv_cache_quant_safety_table);
        check((std::string("safety table: ") + row.backend + " q8_0 matches documented answer").c_str(),
              q8 == row.q8);
        check((std::string("safety table: ") + row.backend + " q4_0 matches documented answer").c_str(),
              q4 == row.q4);
    }
}

static void test_f16_always_safe_on_every_backend() {
    using lemon::backends::llamacpp::kv_cache_quant_safety_table;
    for (const char* backend : {"cuda", "rocm-stable", "rocm-nightly", "metal",
                                "vulkan", "cpu", "system", "totally-unknown-backend"}) {
        check((std::string("safety table: f16 safe on ") + backend).c_str(),
              lemon::kv_cache_quant_backend_eligible(
                  KvCacheQuantTier::F16, backend, kv_cache_quant_safety_table));
    }
}

static void test_unrecognized_backend_never_safe() {
    using lemon::backends::llamacpp::kv_cache_quant_safety_table;
    check("safety table: unrecognized backend q8_0 is false",
          !lemon::kv_cache_quant_backend_eligible(
              KvCacheQuantTier::Q8_0, "totally-unknown-backend", kv_cache_quant_safety_table));
    check("safety table: unrecognized backend q4_0 is false",
          !lemon::kv_cache_quant_backend_eligible(
              KvCacheQuantTier::Q4_0, "totally-unknown-backend", kv_cache_quant_safety_table));
}

static void test_model_gate_divisible_head_dims() {
    check("model gate: 128-wide K/V eligible for q8_0",
          lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q8_0, 128, 128));
    check("model gate: 128-wide K/V eligible for q4_0",
          lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q4_0, 128, 128));
}

static void test_model_gate_indivisible_key_dim() {
    check("model gate: 100-wide K ineligible for q8_0",
          !lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q8_0, 100, 128));
    check("model gate: 100-wide K ineligible for q4_0",
          !lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q4_0, 100, 128));
    check("model gate: 100-wide K still eligible for f16",
          lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::F16, 100, 128));
}

static void test_model_gate_checks_key_and_value_independently() {
    // Key divides, value does not.
    check("model gate: divisible K but indivisible V is ineligible",
          !lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q8_0, 128, 100));
}

static void test_model_gate_zero_value_dim_ineligible() {
    check("model gate: key present, value dim zero (absent) is ineligible",
          !lemon::kv_cache_quant_model_eligible(KvCacheQuantTier::Q8_0, 128, 0));
}

// ── Ladder resolver ───────────────────────────────────────────────────

using lemon::KvCacheResolution;
using lemon::ModelInfo;
using lemon::RecipeOptions;

static ModelInfo make_model(int64_t key_dim, int64_t value_dim, int64_t max_ctx_window = 0,
                            double size_gb = 0.1, bool embedding = false) {
    ModelInfo mi;
    mi.recipe = "llamacpp";
    mi.model_name = "test-model";
    mi.gguf.block_count = 32;
    mi.gguf.head_count_kv = 128;  // uniform across all 32 layers
    mi.gguf.key_length = key_dim;
    mi.gguf.value_length = value_dim;
    mi.max_context_window = max_ctx_window;
    mi.size = size_gb;
    mi.device = lemon::DEVICE_GPU;
    mi.type = embedding ? lemon::ModelType::EMBEDDING : lemon::ModelType::LLM;
    return mi;
}

static RecipeOptions make_opts(const nlohmann::json& overrides) {
    return RecipeOptions("llamacpp", overrides);
}

// Available memory (GB) that makes `tier`'s achieved max context land at
// `target_ctx * headroom` tokens for a uniform 128-head, `key_dim`-wide
// model, using the exact production byte-cost formula.
static double memory_gb_for_ctx(lemon::KvCacheQuantTier tier, int64_t key_dim,
                                double target_ctx, double model_weight_gb,
                                double headroom = 1.0) {
    const double bytes_per_token = 128.0 * static_cast<double>(key_dim) *
                                   lemon::kv_cache_quant_bytes_per_element(tier) * 2.0;
    const double available_for_kv_gb =
        (target_ctx * headroom * bytes_per_token) / lemon::BYTES_PER_GIB;
    return available_for_kv_gb + model_weight_gb;
}

static const lemon::KvCacheQuantSafetyTable kSafeTable = {
    {"cuda", {true, true}},
};
static const lemon::KvCacheQuantSafetyTable kVulkanTable = {
    {"vulkan", {false, false}},
};

static void test_f16_ctx_minus1_matches_compute_auto_context_size() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "f16"}});
    const double mem_gb = 16.0;
    const int64_t expected = lemon::compute_auto_context_size(mi, mem_gb, false);
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: f16 ctx:-1 matches today's compute_auto_context_size exactly",
          res.ok() && res.tier == KvCacheQuantTier::F16 && res.ctx_size == expected &&
          res.ctx_size_is_auto);
}

static void test_auto_selects_f16_when_it_already_reaches_max() {
    // Generous memory, small declared max: f16 alone already clears it.
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, /*mem_gb=*/64.0, "cuda", kSafeTable);
    check("U5: auto does not quantize when f16 already reaches the model max",
          res.ok() && res.tier == KvCacheQuantTier::F16 && !res.structurally_ineligible);
}

static void test_auto_selects_q8_when_f16_falls_short() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q8_0, 128,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, /*headroom=*/1.05);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: auto selects q8_0 when f16 falls short and q8_0 reaches target",
          res.ok() && res.tier == KvCacheQuantTier::Q8_0 &&
          res.ctx_size == lemon::AUTO_CTX_UNKNOWN_MAX);
}

static void test_balanced_floors_at_q8_even_with_q4_min() {
    // Memory too tight for either f16 or q8_0 to reach the target; balanced
    // must never descend to q4_0 even though min_kv_quantization allows it.
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q8_0, 128,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, /*headroom=*/0.5);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "balanced"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    const int64_t expected_q8_ctx = lemon::compute_auto_context_size(
        mi, mem_gb, false, lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q8_0));
    check("U5: balanced never descends to q4_0 regardless of min_kv_quantization",
          res.ok() && res.tier == KvCacheQuantTier::Q8_0 && res.ctx_size == expected_q8_ctx &&
          res.ctx_size_is_auto);
}

static void test_max_context_descends_to_q4_when_q8_falls_short() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    // Tight enough that q8_0 falls short of the target, but q4_0 clears it.
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, /*headroom=*/1.05);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_context"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: max_context descends to q4_0 when q8_0 falls short",
          res.ok() && res.tier == KvCacheQuantTier::Q4_0 &&
          res.ctx_size == lemon::AUTO_CTX_UNKNOWN_MAX);
}

static void test_balanced_with_f16_min_selects_f16_only() {
    // A raised floor (min_kv_quantization: f16) restricts balanced further,
    // even under memory pressure that would otherwise force quantization.
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, /*headroom=*/1.05);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "balanced"},
                                    {"min_kv_quantization", "f16"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    const int64_t expected_f16_ctx = lemon::compute_auto_context_size(mi, mem_gb, false);
    check("U5: balanced with min_kv_quantization: f16 selects f16 only",
          res.ok() && res.tier == KvCacheQuantTier::F16 && res.ctx_size == expected_f16_ctx);
}

static void test_max_speed_selects_f16_regardless() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, /*headroom=*/1.05);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_speed"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    const int64_t expected_f16_ctx = lemon::compute_auto_context_size(mi, mem_gb, false);
    check("U5: max_speed selects f16 regardless of how much quantizing would unlock",
          res.ok() && res.tier == KvCacheQuantTier::F16 && res.ctx_size == expected_f16_ctx &&
          !res.structurally_ineligible);
}

static void test_max_kv_quantization_q8_never_selects_f16() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"max_kv_quantization", "q8_0"}});
    // Generous memory: f16 would trivially reach the target, but the ceiling
    // excludes it from the ladder entirely.
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, /*mem_gb=*/64.0, "cuda", kSafeTable);
    check("U5: max_kv_quantization: q8_0 never selects f16",
          res.ok() && res.tier == KvCacheQuantTier::Q8_0);
}

static void test_max_kv_quantization_below_floor_raises_invalid_argument() {
    // Default balanced priority floors at q8_0 (rank 1) regardless of
    // min_kv_quantization; max_kv_quantization: q4_0 (rank 2) is a ceiling
    // ranked *lower* quality than that floor -- lo(2) > hi(1), an empty and
    // contradictory ladder range. Must reject rather than silently landing
    // on f16 with no signal (the structural-ineligibility flag only
    // covers the backend/model-gate case, not a self-contradictory config).
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"max_kv_quantization", "q4_0"}});
    bool threw = false;
    try {
        lemon::resolve_kv_cache(opts, mi, /*mem_gb=*/64.0, "cuda", kSafeTable);
    } catch (const std::invalid_argument& e) {
        threw = true;
        const std::string what = e.what();
        check("U5: max_kv_quantization-below-floor error names max_kv_quantization",
              what.find("max_kv_quantization") != std::string::npos);
        check("U5: max_kv_quantization-below-floor error names the contradicting tiers",
              what.find("q4_0") != std::string::npos && what.find("q8_0") != std::string::npos);
    }
    check("U5: max_kv_quantization ranked below the priority floor raises std::invalid_argument", threw);
}

static void test_auto_on_vulkan_selects_f16_structurally_ineligible() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"}});
    KvCacheResolution auto_res = lemon::resolve_kv_cache(opts, mi, 64.0, "vulkan", kVulkanTable);
    RecipeOptions f16_opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "f16"}});
    KvCacheResolution f16_res = lemon::resolve_kv_cache(f16_opts, mi, 64.0, "vulkan", kVulkanTable);
    check("U5: auto on vulkan selects f16",
          auto_res.ok() && auto_res.tier == KvCacheQuantTier::F16);
    check("U5: auto on vulkan resolves the same context as plain f16",
          auto_res.ctx_size == f16_res.ctx_size);
    check("U5: auto on vulkan is flagged structurally ineligible (R14)",
          auto_res.structurally_ineligible);
}

static void test_auto_indivisible_head_dim_selects_f16_even_under_pressure() {
    // key_dim=100 is not divisible by 32; safe backend and tight memory would
    // otherwise force a lower tier, but the model gate blocks every tier below f16.
    ModelInfo mi = make_model(/*key_dim=*/100, /*value_dim=*/100, /*max_ctx_window=*/0, 0.1);
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 100,
                                            lemon::AUTO_CTX_UNKNOWN_MAX, 0.1, 1.05);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: indivisible head dim selects f16 even on a safe backend under memory pressure",
          res.ok() && res.tier == KvCacheQuantTier::F16 && res.structurally_ineligible);
}

static void test_explicit_ctx_unfittable_reports_r8_failure() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    // Memory affords only a small context even at q4_0; request far more.
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128, 4096, 0.1, 1.0);
    const int64_t requested = 5'000'000;
    RecipeOptions opts = make_opts({{"ctx_size", requested}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_context"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: unfittable explicit ctx_size reports failure, not a silent reduction",
          !res.ok());
    check("U5: R8 failure names the requested context",
          res.failure.find(std::to_string(requested)) != std::string::npos);
    check("U5: R8 failure names the best (lowest-quality) tier tried",
          res.failure.find("q4_0") != std::string::npos);
}

static void test_explicit_ctx_only_q4_fits_selects_q4_at_requested_value() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    const int64_t requested = 20000;
    // Memory that fits `requested` tokens at q4_0 but not at q8_0 or f16.
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128,
                                            static_cast<double>(requested), 0.1, 1.02);
    RecipeOptions opts = make_opts({{"ctx_size", requested}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_context"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: explicit ctx_size only q4_0 can fit selects q4_0",
          res.ok() && res.tier == KvCacheQuantTier::Q4_0);
    check("U5: no silent context reduction for an explicit request",
          res.ok() && res.ctx_size == requested && !res.ctx_size_is_auto);
}

static void test_ctx_minus1_unsatisfiable_shrinks_and_reports_auto() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1);
    // Even generous headroom at max_kv_quantization stays well under the
    // AUTO_CTX_UNKNOWN_MAX target, so every tier falls short.
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128, 500.0, 0.1, 1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_context"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    const int64_t expected_q4_ctx = lemon::compute_auto_context_size(
        mi, mem_gb, false, lemon::kv_cache_quant_bytes_per_element(KvCacheQuantTier::Q4_0));
    check("U5: ctx_size:-1 exhaustion shrinks to the lowest surviving tier's max",
          res.ok() && res.tier == KvCacheQuantTier::Q4_0 && res.ctx_size == expected_q4_ctx);
    check("U5: shrink-on-exhaustion is still reported as auto-resolved",
          res.ok() && res.ctx_size_is_auto);
}

static void test_flash_attention_conflict_reports_r11_before_success() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "q8_0"},
                                    {"llamacpp_args", "-fa off"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, 64.0, "cuda", kSafeTable);
    check("U5: quant tier + disabled flash attention reports R11 conflict",
          !res.ok());
    check("U5: R11 failure mentions the resolved tier",
          res.failure.find("q8_0") != std::string::npos);
}

static void test_f16_with_flash_attention_disabled_reports_no_conflict() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "f16"},
                                    {"llamacpp_args", "-fa off"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, 64.0, "cuda", kSafeTable);
    check("U5: f16 with flash attention disabled reports no conflict",
          res.ok() && res.tier == KvCacheQuantTier::F16);
}

static void test_embedding_model_keeps_floor_after_shrink() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/0, /*size_gb=*/0.1, /*embedding=*/true);
    // Tight memory: q4_0's raw achievable context is small but positive
    // (tens of tokens), well below EMBEDDING_CTX_SIZE, so the floor clamp
    // must raise it rather than shrink-to-fit stopping short.
    const double mem_gb = memory_gb_for_ctx(KvCacheQuantTier::Q4_0, 128, /*target_ctx=*/100.0, 0.1, 1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"},
                                    {"kv_cache_priority", "max_context"},
                                    {"min_kv_quantization", "q4_0"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, mem_gb, "cuda", kSafeTable);
    check("U5: embedding model keeps at least the embedding context floor after a shrink",
          res.ok() && res.ctx_size >= lemon::EMBEDDING_CTX_SIZE);
}

static void test_undetectable_memory_falls_back_to_f16_fallback() {
    ModelInfo mi = make_model(128, 128, /*max_ctx_window=*/8192, /*size_gb=*/1.0);
    RecipeOptions opts = make_opts({{"ctx_size", -1}, {"kv_cache_quantization", "auto"}});
    KvCacheResolution res = lemon::resolve_kv_cache(opts, mi, /*mem_gb=*/0.0, "cuda", kSafeTable);
    check("U5: undetectable memory falls back to f16 without consulting the ladder",
          res.ok() && res.tier == KvCacheQuantTier::F16 && res.ctx_size == lemon::AUTO_CTX_FALLBACK &&
          !res.structurally_ineligible);
}

static void test_invalid_option_values_raise_before_memory_query() {
    ModelInfo mi = make_model(128, 128);
    struct Case { const char* key; const char* value; };
    const Case cases[] = {
        {"kv_cache_quantization", "q5_1"},
        {"max_kv_quantization", "bogus"},
        {"min_kv_quantization", "bogus"},
        {"kv_cache_priority", "bogus"},
    };
    for (const auto& c : cases) {
        RecipeOptions opts = make_opts({{c.key, c.value}});
        bool threw = false;
        try {
            // NaN memory would corrupt any downstream arithmetic if the
            // validation path ever touched it — proof validation runs first.
            lemon::resolve_kv_cache(opts, mi, std::nan(""), "cuda", kSafeTable);
        } catch (const std::invalid_argument& e) {
            threw = true;
            check((std::string("U5: invalid ") + c.key + " message names the option").c_str(),
                  std::string(e.what()).find(c.key) != std::string::npos);
        }
        check((std::string("U5: invalid ") + c.key + " raises std::invalid_argument").c_str(), threw);
    }
}

int main() {
    test_bytes_per_element_exact_values();
    test_bytes_per_element_not_round_numbers();
    test_block_size();
    test_config_parse_accepts_exact_set();
    test_config_parse_rejects_out_of_set();
    test_round_trip_every_value();
    test_narrowing_auto_is_not_a_concrete_tier();
    test_ladder_order();
    test_gguf_value_length();
    test_weighted_byte_cost_uses_both_key_and_value_length();
    test_weighted_byte_cost_falls_back_value_length_to_key_length_when_absent();
    test_safety_table_documented_answers();
    test_f16_always_safe_on_every_backend();
    test_unrecognized_backend_never_safe();
    test_model_gate_divisible_head_dims();
    test_model_gate_indivisible_key_dim();
    test_model_gate_checks_key_and_value_independently();
    test_model_gate_zero_value_dim_ineligible();
    test_f16_ctx_minus1_matches_compute_auto_context_size();
    test_auto_selects_f16_when_it_already_reaches_max();
    test_auto_selects_q8_when_f16_falls_short();
    test_balanced_floors_at_q8_even_with_q4_min();
    test_max_context_descends_to_q4_when_q8_falls_short();
    test_balanced_with_f16_min_selects_f16_only();
    test_max_speed_selects_f16_regardless();
    test_max_kv_quantization_q8_never_selects_f16();
    test_max_kv_quantization_below_floor_raises_invalid_argument();
    test_auto_on_vulkan_selects_f16_structurally_ineligible();
    test_auto_indivisible_head_dim_selects_f16_even_under_pressure();
    test_explicit_ctx_unfittable_reports_r8_failure();
    test_explicit_ctx_only_q4_fits_selects_q4_at_requested_value();
    test_ctx_minus1_unsatisfiable_shrinks_and_reports_auto();
    test_flash_attention_conflict_reports_r11_before_success();
    test_f16_with_flash_attention_disabled_reports_no_conflict();
    test_embedding_model_keeps_floor_after_shrink();
    test_undetectable_memory_falls_back_to_f16_fallback();
    test_invalid_option_values_raise_before_memory_query();

    if (g_failures == 0) {
        std::printf("\nAll kv_cache_quant tests passed\n");
        return 0;
    }
    std::printf("\n%d kv_cache_quant test(s) FAILED\n", g_failures);
    return 1;
}
