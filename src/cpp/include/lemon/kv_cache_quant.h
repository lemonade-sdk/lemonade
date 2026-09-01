#pragma once

#include <optional>
#include <string>

namespace lemon {

// User-facing value of the `kv_cache_quantization` recipe option. Four
// values: f16 (default, unchanged behavior), auto (ladder-walk to fit a
// target context), or an explicit quant tier applied directly. This is a
// distinct type from KvCacheQuantTier because `auto` is a request to choose,
// not a thing that can be quantized with (KTD11).
enum class KvCacheQuantConfig {
    F16,
    Auto,
    Q8_0,
    Q4_0,
};

// A concrete, resolvable KV cache quantization tier. Every value here has a
// bytes-per-element factor and a ggml block size; KvCacheQuantConfig::Auto
// has neither, so narrowing it to this type is an explicit, checked step
// (kv_cache_quant_tier_from_config) rather than a silent default.
enum class KvCacheQuantTier {
    F16,
    Q8_0,
    Q4_0,
};

inline std::string kv_cache_quant_config_to_string(KvCacheQuantConfig value) {
    switch (value) {
        case KvCacheQuantConfig::F16:  return "f16";
        case KvCacheQuantConfig::Auto: return "auto";
        case KvCacheQuantConfig::Q8_0: return "q8_0";
        case KvCacheQuantConfig::Q4_0: return "q4_0";
    }
    return "f16";
}

// Accepts exactly the four values R1 defines. Rejects llama.cpp's wider
// KV-cache type set (q5_1, etc.), wrong-case variants, and empty strings —
// widening this set is deferred (see plan Deferred to Follow-Up Work).
inline std::optional<KvCacheQuantConfig> parse_kv_cache_quant_config(const std::string& value) {
    if (value == "f16")  return KvCacheQuantConfig::F16;
    if (value == "auto") return KvCacheQuantConfig::Auto;
    if (value == "q8_0") return KvCacheQuantConfig::Q8_0;
    if (value == "q4_0") return KvCacheQuantConfig::Q4_0;
    return std::nullopt;
}

inline std::string kv_cache_quant_tier_to_string(KvCacheQuantTier value) {
    switch (value) {
        case KvCacheQuantTier::F16:  return "f16";
        case KvCacheQuantTier::Q8_0: return "q8_0";
        case KvCacheQuantTier::Q4_0: return "q4_0";
    }
    return "f16";
}

inline std::optional<KvCacheQuantTier> parse_kv_cache_quant_tier(const std::string& value) {
    if (value == "f16")  return KvCacheQuantTier::F16;
    if (value == "q8_0") return KvCacheQuantTier::Q8_0;
    if (value == "q4_0") return KvCacheQuantTier::Q4_0;
    return std::nullopt;
}

// Bytes per element, derived as the tier's ggml block byte size over its
// block element count rather than a round power of two: a quantized block
// stores a scale alongside its packed values, so q8_0 costs 1.0625 bytes per
// element and q4_0 costs 0.5625 (KTD1) — not 1.0 and 0.5. Using the round
// numbers would under-reserve KV memory by 6.25% and 12.5% respectively.
inline double kv_cache_quant_bytes_per_element(KvCacheQuantTier tier) {
    switch (tier) {
        case KvCacheQuantTier::F16:  return 2.0 / 1.0;
        case KvCacheQuantTier::Q8_0: return 34.0 / 32.0;
        case KvCacheQuantTier::Q4_0: return 18.0 / 32.0;
    }
    return 2.0;
}

// ggml block size, in elements. f16 is unquantized (block size 1), so every
// head dimension trivially divides it; q8_0 and q4_0 blocks are 32 elements,
// which is what R9's head-dimension eligibility gate checks against.
inline int kv_cache_quant_block_size(KvCacheQuantTier tier) {
    switch (tier) {
        case KvCacheQuantTier::F16:  return 1;
        case KvCacheQuantTier::Q8_0: return 32;
        case KvCacheQuantTier::Q4_0: return 32;
    }
    return 1;
}

// Narrows a config value to a resolved tier. Returns std::nullopt for
// KvCacheQuantConfig::Auto: a request to choose is not a concrete tier, and
// this makes that case explicit at every call site instead of letting it
// silently reach bytes-per-element, the block-size lookup, or the safety
// table (KTD11).
inline std::optional<KvCacheQuantTier> kv_cache_quant_tier_from_config(KvCacheQuantConfig config) {
    switch (config) {
        case KvCacheQuantConfig::F16:  return KvCacheQuantTier::F16;
        case KvCacheQuantConfig::Auto: return std::nullopt;
        case KvCacheQuantConfig::Q8_0: return KvCacheQuantTier::Q8_0;
        case KvCacheQuantConfig::Q4_0: return KvCacheQuantTier::Q4_0;
    }
    return std::nullopt;
}

// Ladder rank, ascending in quantization (descending in quality): f16 < q8_0
// < q4_0. The single ordering authority the ladder walk and the
// kv_cache_priority floor comparisons in U5 both use.
inline int kv_cache_quant_tier_rank(KvCacheQuantTier tier) {
    switch (tier) {
        case KvCacheQuantTier::F16:  return 0;
        case KvCacheQuantTier::Q8_0: return 1;
        case KvCacheQuantTier::Q4_0: return 2;
    }
    return 0;
}

// True when `a` is strictly higher quality (less quantized) than `b`.
inline bool kv_cache_quant_tier_higher_quality(KvCacheQuantTier a, KvCacheQuantTier b) {
    return kv_cache_quant_tier_rank(a) < kv_cache_quant_tier_rank(b);
}

} // namespace lemon
