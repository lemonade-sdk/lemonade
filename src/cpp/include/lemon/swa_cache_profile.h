#pragma once

// Conservative, F16-only estimate of the *allocated* llama.cpp ISWA KV cache.
// Keep backend compatibility separate from GGUF pattern discovery: SWA metadata
// does not imply that an architecture actually uses llama_kv_cache_iswa.

#include <lemon/gguf_reader.h>
#include <lemon/utils/custom_args.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace lemon {

struct BoundedSwaProfile {
    double full_bytes_per_token = 0.0;
    double swa_bytes_per_cell = 0.0;
    int64_t window = 0;

    static constexpr int64_t kBatchMargin = 4096;
    static constexpr int64_t kAlignment = 256;

    static int64_t pad_ctx(int64_t ctx) {
        if (ctx <= 0 || ctx > (std::numeric_limits<int64_t>::max)() - kAlignment)
            return 0;
        return ((ctx + kAlignment - 1) / kAlignment) * kAlignment;
    }

    int64_t swa_cells(int64_t ctx) const {
        const int64_t base = pad_ctx(ctx);
        if (base <= 0 || window <= 0 ||
            window > (std::numeric_limits<int64_t>::max)() - kBatchMargin)
            return 0;
        return pad_ctx((std::min)(base, window + kBatchMargin));
    }

    double bytes_for_context(int64_t ctx) const {
        const int64_t base = pad_ctx(ctx);
        const int64_t swa = swa_cells(ctx);
        if (base <= 0 || swa <= 0) return std::numeric_limits<double>::infinity();
        return full_bytes_per_token * static_cast<double>(base) +
               swa_bytes_per_cell * static_cast<double>(swa);
    }

    double unbounded_bytes_per_token() const {
        return full_bytes_per_token + swa_bytes_per_cell;
    }
};

namespace swa_detail {

struct ArchitectureLayout {
    int64_t period = -1; // -1 means an explicit pattern is required
    bool dense_first = false;
    int64_t default_window = 0;
};

// These are the load_swa_pattern() defaults in llama.cpp's architecture
inline std::optional<ArchitectureLayout> layout_for(const GgufMetadata& m,
                                                     const std::string& recipe) {
    if (recipe != "llamacpp" && recipe != "llamacpp-hrx") return std::nullopt;
    const auto& a = m.architecture;
    if (a == "gemma2")       return ArchitectureLayout{2, false, 4096};
    if (a == "gemma3")       return ArchitectureLayout{6, false, 0};
    if (a == "gemma4")       return ArchitectureLayout{-1, false, 0};
    if (a == "gpt-oss")   return ArchitectureLayout{2, false, 0}; // GPT-OSS
    if (a == "cohere2")      return ArchitectureLayout{4, false, 0};
    if (a == "cohere2moe")   return ArchitectureLayout{4, true, 0};
    if (a == "olmo2")        return ArchitectureLayout{4, false, 0}; // OLMo 3 SWA
    if (a == "exaone4" && m.block_count == 64)
                              return ArchitectureLayout{4, false, 4096};
    // Granite SWA's explicit pattern is supported in current llama.cpp, but
    // the pinned HRX-b99 fork predates that architecture loader.
    if (a == "granite_swa" && recipe == "llamacpp")
                              return ArchitectureLayout{-1, false, 0};
    return std::nullopt;
}

inline bool is_parallel_one(const std::string& value) {
    if (value.empty()) return false;
    int64_t v = 0;
    for (char c : value) {
        if (c < '0' || c > '9' || v > 10) return false;
        v = v * 10 + (c - '0');
    }
    return v == 1;
}

} // namespace swa_detail

// Do not infer bounded storage from theoretical SWA or from a malformed
// per-layer GGUF pattern. In particular, an explicitly invalid pattern must
// NOT be replaced with an architecture default.
inline std::optional<BoundedSwaProfile> compute_bounded_swa_profile(
    const GgufMetadata& m, const std::string& recipe) {
    const auto layout = swa_detail::layout_for(m, recipe);
    if (!layout || m.block_count <= 0 || m.block_count > 8192 ||
        m.full_attention_interval > 1 || m.shared_kv_layers != 0 ||
        !m.sliding_window_pattern_valid || !m.bounded_swa_metadata_valid ||
        m.head_count_kv_per_layer.size() != static_cast<size_t>(m.block_count))
        return std::nullopt;

    const int64_t window = m.sliding_window > 0 ? m.sliding_window : layout->default_window;
    if (window <= 0 || window > INT32_MAX) return std::nullopt;

    const bool explicit_array = !m.sliding_window_pattern.empty();
    if (explicit_array &&
        m.sliding_window_pattern.size() != static_cast<size_t>(m.block_count))
        return std::nullopt;
    if (!explicit_array && m.sliding_window_pattern_present &&
        !m.sliding_window_pattern_scalar_present)
        return std::nullopt;
    const int64_t period = m.sliding_window_pattern_scalar_present
        ? m.sliding_window_pattern_period : layout->period;
    if (!explicit_array && (period < 0 || period > m.block_count))
        return std::nullopt;

    // llama-model.cpp initializes missing K/V head dimensions from
    // embedding_length/head_count. Do not guess V=K when the model might
    // have asymmetric dimensions (e.g. K=512, V=128).
    const int64_t default_dim = (m.head_count > 0 && m.embedding_length > 0 &&
        m.embedding_length % m.head_count == 0)
        ? m.embedding_length / m.head_count : 0;
    const int64_t key_full = m.key_length > 0 ? m.key_length : default_dim;
    const int64_t value_full = m.value_length > 0 ? m.value_length : default_dim;
    const int64_t key_swa = m.key_length_swa > 0 ? m.key_length_swa : key_full;
    const int64_t value_swa = m.value_length_swa > 0 ? m.value_length_swa : value_full;
    if (key_full <= 0 || value_full <= 0 || key_swa <= 0 || value_swa <= 0)
        return std::nullopt;

    std::vector<bool> pattern;
    if (explicit_array) {
        pattern = m.sliding_window_pattern;
    } else {
        pattern.resize(static_cast<size_t>(m.block_count));
        for (int64_t i = 0; i < m.block_count; ++i) {
            pattern[static_cast<size_t>(i)] = period == 0 ||
                (layout->dense_first ? i % period != 0 : i % period < period - 1);
        }
    }

    // Without flash attention llama_kv_cache pads *all* V layer rows to
    // n_embd_v_gqa_max(). Always charge that worst case: cache-layout flags
    // cannot make this estimate too small, whether FA is on or off.
    int64_t max_v_gqa = 0;
    int64_t head_sum = 0;
    bool any_swa = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const int64_t heads = m.head_count_kv_per_layer[i];
        if (heads <= 0 || heads > INT32_MAX ||
            heads > (std::numeric_limits<int64_t>::max)() / (std::max)(value_full, value_swa))
            return std::nullopt;
        if (head_sum > INT64_MAX - heads) return std::nullopt;
        head_sum += heads;
        max_v_gqa = (std::max)(max_v_gqa,
            heads * (pattern[i] ? value_swa : value_full));
        any_swa |= pattern[i];
    }
    if (!any_swa || (m.head_count_kv > 0 && m.head_count_kv != head_sum))
        return std::nullopt;

    BoundedSwaProfile result;
    result.window = window;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const double k = static_cast<double>(m.head_count_kv_per_layer[i]) *
                         static_cast<double>(pattern[i] ? key_swa : key_full);
        const double bytes = 2.0 * (k + static_cast<double>(max_v_gqa)); // F16 K and V
        if (pattern[i]) result.swa_bytes_per_cell += bytes;
        else result.full_bytes_per_token += bytes;
    }
    if (!std::isfinite(result.unbounded_bytes_per_token()) ||
        result.swa_bytes_per_cell <= 0.0) return std::nullopt;
    return result;
}

// Return zero if even the minimum (one cell, padded) does not fit. Supports
// pure SWA with full_bytes_per_token == 0 without an artificial slope.
inline int64_t bounded_swa_context_limit(const BoundedSwaProfile& profile,
                                        double kv_budget_gib,
                                        double full_bytes_per_token,
                                        double swa_bytes_per_cell,
                                        int64_t max_context) {
    if (!std::isfinite(kv_budget_gib) || kv_budget_gib <= 0.0 ||
        !std::isfinite(full_bytes_per_token) || full_bytes_per_token < 0.0 ||
        !std::isfinite(swa_bytes_per_cell) || swa_bytes_per_cell <= 0.0 ||
        max_context <= 0) return 0;
    const double budget_bytes = kv_budget_gib * 1024.0 * 1024.0 * 1024.0;
    auto fits = [&](int64_t tokens) {
        const int64_t cells = profile.swa_cells(tokens);
        const int64_t base = BoundedSwaProfile::pad_ctx(tokens);
        return cells > 0 && base > 0 &&
            full_bytes_per_token * base + swa_bytes_per_cell * cells <= budget_bytes;
    };
    int64_t low = 0, high = (std::min)(max_context, static_cast<int64_t>(INT32_MAX));
    while (low < high) {
        const int64_t mid = low + (high - low + 1) / 2;
        if (fits(mid)) low = mid;
        else high = mid - 1;
    }
    return low;
}

// Both recipes pin --parallel 1 unless overridden by custom arguments.
// Unknown overrides that change cache allocation deliberately fall back.
inline bool bounded_swa_runtime_compatible(const std::string& custom_args) {
    const char* parallel_env = std::getenv("LLAMA_ARG_N_PARALLEL");
    if (parallel_env && !swa_detail::is_parallel_one(parallel_env)) return false;
    for (const char* name : {"LLAMA_ARG_SWA_FULL", "LLAMA_ARG_UBATCH",
                             "LLAMA_ARG_CACHE_TYPE_K", "LLAMA_ARG_CACHE_TYPE_V",
                             "LLAMA_ARG_KV_UNIFIED"}) {
        if (std::getenv(name)) return false;
    }

    const auto args = utils::parse_custom_args(custom_args);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& token = args[i];
        const auto eq = token.find('=');
        const std::string flag = token.substr(0, eq);
        if (flag == "--swa-full" || flag == "--no-swa-full" ||
            flag == "--ubatch-size" || flag == "-ub" ||
            flag == "--cache-type-k" || flag == "-ctk" ||
            flag == "--cache-type-v" || flag == "-ctv" ||
            flag == "--kv-unified" || flag == "--no-kv-unified" ||
            flag == "-kvu") return false;
        if (flag == "--parallel" || flag == "-np") {
            const std::string value = eq != std::string::npos
                ? token.substr(eq + 1) : (i + 1 < args.size() ? args[++i] : "");
            if (!swa_detail::is_parallel_one(value)) return false;
        }
    }
    return true;
}

inline bool bounded_swa_runtime_compatible_for_recipe(
    const std::string& recipe, const std::string& llamacpp_args,
    const std::string& hrx_args) {
    if (recipe == "llamacpp") return bounded_swa_runtime_compatible(llamacpp_args);
    if (recipe == "llamacpp-hrx") return bounded_swa_runtime_compatible(hrx_args);
    return false;
}

} // namespace lemon
