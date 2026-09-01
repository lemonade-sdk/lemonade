// Standalone test for the KV-cache quant tier vocabulary (U1), the GGUF
// value_length field it depends on, and the safety table + eligibility gates
// (U4).
//
// Compile: g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include test/cpp/test_kv_cache_quant.cpp -o test_kv_cache_quant

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
    // Direct guard against the round-number regression KTD1 exists to prevent:
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
    test_safety_table_documented_answers();
    test_f16_always_safe_on_every_backend();
    test_unrecognized_backend_never_safe();
    test_model_gate_divisible_head_dims();
    test_model_gate_indivisible_key_dim();
    test_model_gate_checks_key_and_value_independently();
    test_model_gate_zero_value_dim_ineligible();

    if (g_failures == 0) {
        std::printf("\nAll kv_cache_quant tests passed\n");
        return 0;
    }
    std::printf("\n%d kv_cache_quant test(s) FAILED\n", g_failures);
    return 1;
}
