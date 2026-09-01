#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <lemon/kv_cache_quant.h>
#include <lemon/model_manager.h>
#include <lemon/system_info.h>
#include <lemon/system_metrics_platform.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/custom_args.h>

namespace lemon {

// Minimum context size for embedding models (applied as a floor after auto-resolution)
constexpr int64_t EMBEDDING_CTX_SIZE = 8192;

// Fallback context size when auto-resolution cannot be computed
constexpr int64_t AUTO_CTX_FALLBACK = 4096;

// Hard cap on context size when the model's max context window is unknown.
// No modern model supports less than this, and it prevents runaway allocation.
constexpr int64_t AUTO_CTX_UNKNOWN_MAX = 32768;

// Bytes per GiB (for unit conversion: GB → bytes)
constexpr double BYTES_PER_GIB = 1024.0 * 1024.0 * 1024.0;

/// Estimate KV cache bytes-per-token from model size when GGUF metadata is unavailable.
/// Assumes F16 KV cache, 16 KV heads, 128 head-dim across all model sizes.
/// 16 KV heads is a conservative upper bound (most models use 2–8 with GQA).
///
/// Formula: layers × 16(kv_heads) × 128(head_dim) × 2[F16] × 2[K+V] = layers × 8192
///   <1B   (12 layers)  → ~128 KB/token
///   ~3B   (28 layers)  → ~224 KB/token
///   ~7B   (32 layers)  → ~256 KB/token
///   ~14B  (40 layers)  → ~320 KB/token
///   ~32B  (64 layers)  → ~512 KB/token
///   ~70B  (80 layers)  → ~640 KB/token
///   ~100B+ (96 layers) → ~768 KB/token
///
/// The buckets above are derived assuming 2 bytes per KV element (F16); a
/// non-default bytes_per_element scales the result by bytes_per_element/2.0.
static double estimate_kv_bytes_per_token_from_model_size(double model_size_gb,
                                                           double bytes_per_element = 2.0) {
    // Layer count scales with model size; 16 KV heads assumed uniformly.
    double f16_bytes_per_token;
    if (model_size_gb < 1.0) {
        // Tiny model (< 1B)
        f16_bytes_per_token = 128.0 * 1024.0;  // 128 KB/token (12 layers)
    } else if (model_size_gb < 3.0) {
        // ~3B class
        f16_bytes_per_token = 224.0 * 1024.0;  // 224 KB/token (28 layers)
    } else if (model_size_gb < 8.0) {
        // ~7B class
        f16_bytes_per_token = 256.0 * 1024.0;  // 256 KB/token (32 layers)
    } else if (model_size_gb < 16.0) {
        // ~14B class
        f16_bytes_per_token = 320.0 * 1024.0;  // 320 KB/token (40 layers)
    } else if (model_size_gb < 32.0) {
        // ~32B class
        f16_bytes_per_token = 512.0 * 1024.0; // 512 KB/token (64 layers)
    } else if (model_size_gb < 64.0) {
        // ~70B class
        f16_bytes_per_token = 640.0 * 1024.0; // 640 KB/token (80 layers)
    } else {
        // 100B+
        f16_bytes_per_token = 768.0 * 1024.0; // 768 KB/token (96 layers)
    }
    return f16_bytes_per_token * (bytes_per_element / 2.0);
}

/// Get the amount of memory currently in use by the platform.
/// For GPU: VRAM in use. For CPU/NPU: system RAM in use.
/// Returns 0.0 if not measurable.
static double get_used_memory_gb(DeviceType device_type) {
    auto metrics = create_metrics_platform();
    if (!metrics) return 0.0;

    if (device_type & DEVICE_GPU) {
        double vram_used = metrics->get_vram_usage_gb();
        if (vram_used > 0) return vram_used;
    }

    // CPU / NPU / fallback: system RAM in use
    double ram_used = metrics->get_memory_usage_gb();
    if (ram_used > 0) return ram_used;

    return 0.0;
}

/// Extract available memory (in GB) for the device targeted by the model.
/// GPU  → VRAM (+ GTT for iGPU) minus currently-used VRAM
/// CPU  → system RAM minus currently-used RAM
/// NPU  → system RAM minus currently-used RAM
inline double get_available_memory_gb(DeviceType device_type) {
    auto si = create_system_info();

    // Subtract currently-used memory
    double used_gb = get_used_memory_gb(device_type);

    // GPU recipes: use VRAM
    if (device_type & DEVICE_GPU) {
        // AMD iGPU (APU — uses dedicated VRAM + GTT from system RAM)
        auto amd_igpu = si->get_amd_igpu_device();
        if (amd_igpu.available && amd_igpu.vram_gb > 0) {
            // iGPU total = dedicated VRAM + GTT (system memory pool accessible by GPU)
            double total_gb = amd_igpu.vram_gb + amd_igpu.virtual_gb;
            double available = (std::max)(0.0, total_gb - used_gb);
            LOG(DEBUG, "AutoTune") << "get_available_memory_gb: GPU (AMD iGPU) total="
                                   << std::fixed << std::setprecision(2) << total_gb
                                   << " GB (vram=" << amd_igpu.vram_gb
                                   << " + gtt=" << amd_igpu.virtual_gb << "), used=" << used_gb
                                   << " GB → " << available << " GB available"  << " ";
            return available;
        }

        // AMD dGPU
        auto amd_dgpus = si->get_amd_dgpu_devices();
        for (const auto& gpu : amd_dgpus) {
            if (gpu.available && gpu.vram_gb > 0) {
                double available = (std::max)(0.0, gpu.vram_gb - used_gb);
                LOG(DEBUG, "AutoTune") << "get_available_memory_gb: GPU (AMD dGPU) total="
                                       << std::fixed << std::setprecision(2) << gpu.vram_gb
                                       << " GB, used=" << used_gb
                                       << " GB → " << available << " GB available"  << " ";
                return available;
            }
        }

        // NVIDIA
        auto nvidia_gpus = si->get_nvidia_gpu_devices();
        for (const auto& gpu : nvidia_gpus) {
            if (gpu.available && gpu.vram_gb > 0) {
                double available = (std::max)(0.0, gpu.vram_gb - used_gb);
                LOG(DEBUG, "AutoTune") << "get_available_memory_gb: GPU (NVIDIA) total="
                                       << std::fixed << std::setprecision(2) << gpu.vram_gb
                                       << " GB, used=" << used_gb
                                       << " GB → " << available << " GB available"  << " ";
                return available;
            }
        }

        // Metal (macOS — Apple Silicon unified memory). CPU and GPU share one pool:
        //   vram_gb    = Metal's recommended GPU working-set budget (a soft ceiling)
        //   virtual_gb = total unified RAM
        // Available to the GPU = the free unified RAM (total − used), capped at the
        // working-set budget so we don't push the system into swap.
        auto apple = si->get_apple_silicon_device();
        if (apple.available && apple.vram_gb > 0) {
            double free_unified = (std::max)(0.0, apple.virtual_gb - used_gb);
            double available = (std::min)(apple.vram_gb, free_unified);
            LOG(DEBUG, "AutoTune") << "get_available_memory_gb: GPU (Metal) budget="
                                   << std::fixed << std::setprecision(2) << apple.vram_gb
                                   << " GB, unified=" << apple.virtual_gb
                                   << " GB, used=" << used_gb
                                   << " GB → " << available << " GB available"  << " ";
            return available;
        }
    }

    // CPU / NPU: use system RAM
    // Apple Silicon: the full unified-memory pool (total physical RAM) is reported
    // as virtual_gb on the Apple Silicon device.
    auto apple = si->get_apple_silicon_device();
    if (apple.available && apple.virtual_gb > 0) {
        double available = (std::max)(0.0, apple.virtual_gb - used_gb);
        LOG(DEBUG, "AutoTune") << "get_available_memory_gb: CPU/NPU (Apple Silicon unified) total="
                               << std::fixed << std::setprecision(2) << apple.virtual_gb
                               << " GB, used=" << used_gb
                               << " GB → " << available << " GB available"  << " ";
        return available;
    }

    // On unified-memory systems (APU), the iGPU vram_gb + virtual_gb approximates system RAM
    auto amd_igpu = si->get_amd_igpu_device();
    if (amd_igpu.available && amd_igpu.vram_gb > 0) {
        double total_gb = amd_igpu.vram_gb + amd_igpu.virtual_gb;
        double available = (std::max)(0.0, total_gb - used_gb);
        LOG(DEBUG, "AutoTune") << "get_available_memory_gb: CPU/NPU (AMD iGPU proxy) total="
                               << std::fixed << std::setprecision(2) << total_gb
                               << " GB, used=" << used_gb
                               << " GB → " << available << " GB available"  << " ";
        return available;
    }

    // Try to get system RAM from dGPU virtual memory (GTT) as a proxy
    auto amd_dgpus = si->get_amd_dgpu_devices();
    for (const auto& gpu : amd_dgpus) {
        if (gpu.available && gpu.virtual_gb > 0) {
            double available = (std::max)(0.0, gpu.virtual_gb - used_gb);
            LOG(DEBUG, "AutoTune") << "get_available_memory_gb: CPU/NPU (AMD dGPU GTT proxy) total="
                                   << std::fixed << std::setprecision(2) << gpu.virtual_gb
                                   << " GB, used=" << used_gb
                                   << " GB → " << available << " GB available"  << " ";
            return available;
        }
    }

    LOG(DEBUG, "AutoTune") << "get_available_memory_gb: could not determine memory, returning 0.0";
    return 0.0;  // Could not determine
}
inline int64_t compute_auto_context_size(const ModelInfo& model_info,
                                          double available_memory_gb,
                                          bool is_embedding = false,
                                          double bytes_per_element = 2.0) {
    if (available_memory_gb <= 0) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — not enough memory, returning " << AUTO_CTX_FALLBACK  << " ";
        return AUTO_CTX_FALLBACK;
    }

    // KV cache bytes per token
    double kv_bytes_per_token = 0;
    double kv_cache_scale = 1.0;
    bool estimated = false;

    // Try exact GGUF metadata first
    int64_t block_count = model_info.gguf.block_count;
    int64_t head_count_kv = model_info.gguf.head_count_kv;
    int64_t key_length = model_info.gguf.key_length;

    if (block_count > 0 && head_count_kv > 0 && key_length > 0) {
        kv_bytes_per_token = compute_weighted_kv_cache_bytes_per_token(
            model_info.gguf, &kv_cache_scale, bytes_per_element);
        if (kv_cache_scale < 1.0) {
            estimated = true;  // mark as architecture-adjusted
        }
        // Log precise SWA head breakdown when raw arrays are available
        // (and both arrays have matching lengths so per-layer indexing is safe)
        if (model_info.gguf.key_length_swa > 0 &&
            model_info.gguf.key_length_swa < model_info.gguf.key_length &&
            !model_info.gguf.head_count_kv_per_layer.empty() &&
            !model_info.gguf.sliding_window_pattern.empty() &&
            model_info.gguf.head_count_kv_per_layer.size() == model_info.gguf.sliding_window_pattern.size()) {
            int64_t swa_heads = 0, full_heads = 0;
            for (size_t i = 0; i < model_info.gguf.head_count_kv_per_layer.size(); ++i) {
                if (model_info.gguf.sliding_window_pattern[i])
                    swa_heads += model_info.gguf.head_count_kv_per_layer[i];
                else
                    full_heads += model_info.gguf.head_count_kv_per_layer[i];
            }
            if (swa_heads > 0 || full_heads > 0) {
                LOG(DEBUG, "AutoTune") << "  SWA head breakdown: swa_heads="
                                       << swa_heads << ", full_heads="
                                       << full_heads << " ";
            }
        }
    } else {
        // GGUF metadata missing — estimate from model size
        kv_bytes_per_token = estimate_kv_bytes_per_token_from_model_size(
            model_info.size, bytes_per_element);
        estimated = true;
    }

    if (kv_bytes_per_token <= 0) {
        return AUTO_CTX_FALLBACK;
    }

    // Available memory for KV cache = total - used - model weights
    // (used is already subtracted in get_available_memory_gb)
    double model_weight_gb = (std::max)(0.0, model_info.size);
    double available_for_kv_gb = available_memory_gb - model_weight_gb;

    if (available_for_kv_gb <= 0) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — no memory for KV after weights (" << std::fixed
                               << std::setprecision(2) << model_weight_gb
                               << " GB), returning " << AUTO_CTX_FALLBACK  << " ";
        return AUTO_CTX_FALLBACK;
    }

    double available_bytes = available_for_kv_gb * BYTES_PER_GIB;
    int64_t max_ctx_from_memory = static_cast<int64_t>(std::floor(available_bytes / kv_bytes_per_token));

    if (max_ctx_from_memory <= 0) {
        return AUTO_CTX_FALLBACK;
    }

    // Clamp to model's declared maximum context window
    int64_t ctx_size = max_ctx_from_memory;
    std::string clamp_note;
    if (model_info.max_context_window > 0 && ctx_size > model_info.max_context_window) {
        ctx_size = model_info.max_context_window;
        clamp_note = " (clamped to model max)";
    } else if (model_info.max_context_window <= 0 && ctx_size > AUTO_CTX_UNKNOWN_MAX) {
        ctx_size = AUTO_CTX_UNKNOWN_MAX;
        clamp_note = " (clamped to unknown-max default)";
    }

    // Embedding models need at least EMBEDDING_CTX_SIZE
    if (is_embedding && ctx_size < EMBEDDING_CTX_SIZE) {
        ctx_size = EMBEDDING_CTX_SIZE;
        clamp_note = " (raised to embedding floor)";
    }

    LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                           << " — GGUF: " << model_info.gguf.architecture
                           << ", blocks=" << block_count << ", kv_heads=" << head_count_kv
                           << ", key_len=" << key_length
                           << ", swa_layers=" << model_info.gguf.swa_layer_count
                           << ", scale=" << std::fixed << std::setprecision(2) << kv_cache_scale
                           << " | kv_cache=" << std::fixed << std::setprecision(2)
                           << (kv_bytes_per_token / (1024.0 * 1024.0)) << " MB/token"
                           << (estimated ? " (est)" : "")
                           << " | memory: " << available_memory_gb << " GB avail, "
                           << model_weight_gb << " GB weights → " << available_for_kv_gb << " GB for KV"
                           << " | ctx=" << max_ctx_from_memory << " → " << ctx_size
                           << clamp_note << " ";
    return ctx_size;
}

// ── KV cache quantization ladder resolver (R3-R11, KTD3) ──────────────────

// Bias values for kv_cache_priority (R3): which end of the context-vs-
// throughput axis the ladder favors. Distinct from the tier types because it
// drives ladder-floor selection, not a quantization amount.
enum class KvCachePriority {
    MaxContext,
    Balanced,
    MaxSpeed,
};

inline std::optional<KvCachePriority> parse_kv_cache_priority(const std::string& value) {
    if (value == "max_context") return KvCachePriority::MaxContext;
    if (value == "balanced")    return KvCachePriority::Balanced;
    if (value == "max_speed")   return KvCachePriority::MaxSpeed;
    return std::nullopt;
}

// Result of resolving both context size and KV cache quant tier together.
// Replaces resolve_auto_ctx_size's bare int64_t / -2-sentinel contract:
// `ctx_size_is_auto` carries what -2 used to mean, and `failure` carries
// R8/R11's resolve-time failures, which callers must raise before
// constructing a backend server rather than let surface from inside load()
// (KTD13).
struct KvCacheResolution {
    KvCacheQuantTier tier = KvCacheQuantTier::F16;
    int64_t ctx_size = 0;
    bool ctx_size_is_auto = false;

    // R14: true when the ladder was entered (auto or an explicit tier) but
    // zero candidates survived the backend/model gates — a structural
    // property of the backend or model, not a memory-fit shortfall. The
    // tier still resolves to f16 in this case; this flag is what makes that
    // outcome distinguishable from an ordinary f16 resolution.
    bool structurally_ineligible = false;

    // Non-empty when resolution failed: R8 (an explicit ctx_size that no
    // eligible tier can fit) or R11 (a quantized tier would be selected but
    // llamacpp_args explicitly disables flash attention). `tier`/`ctx_size`
    // carry no meaning when this is set.
    std::string failure;

    bool ok() const { return failure.empty(); }
};

namespace kv_cache_quant_detail {

inline std::string kv_cache_option_string(const RecipeOptions& options, const char* key,
                                          const std::string& fallback) {
    json v = options.get_option(key);
    return v.is_string() ? v.get<std::string>() : fallback;
}

// True when llamacpp_args explicitly sets -fa/--flash-attn to an off-like
// value. A bare flag (no value) or an on/auto value is not a conflict.
inline bool llamacpp_args_disable_flash_attention(const std::string& llamacpp_args) {
    auto tokens = utils::parse_custom_args(llamacpp_args);
    auto args_map = utils::build_custom_args_map(tokens);
    for (const char* flag : {"-fa", "--flash-attn"}) {
        auto it = args_map.find(flag);
        if (it == args_map.end()) continue;
        for (const auto& occurrence : it->second) {
            if (occurrence.empty()) continue;  // bare flag: not a disable
            const std::string value = gguf_reader_detail::to_lower(occurrence[0]);
            if (value == "off" || value == "0" || value == "false" || value == "no") {
                return true;
            }
        }
    }
    return false;
}

// The f16 path is exactly today's resolve_auto_ctx_size/compute_auto_context_size
// behavior: unconstrained by any target, no R7/R8/R11 semantics. Both the
// plain `kv_cache_quantization: f16` config and every "no eligible tier"
// outcome land here.
inline KvCacheResolution resolve_kv_cache_f16_only(const RecipeOptions& effective_options,
                                                   const ModelInfo& model_info,
                                                   double available_memory_gb,
                                                   bool structurally_ineligible) {
    KvCacheResolution result;
    result.tier = KvCacheQuantTier::F16;
    result.structurally_ineligible = structurally_ineligible;

    json ctx_json = effective_options.get_option("ctx_size");
    int64_t ctx_size = ctx_json.is_number() ? ctx_json.get<int64_t>() : -1;

    if (ctx_size != -1) {
        result.ctx_size = ctx_size;
        result.ctx_size_is_auto = false;
        return result;
    }

    bool is_embedding = (model_info.type == ModelType::EMBEDDING);
    result.ctx_size_is_auto = true;

    if (available_memory_gb <= 0) {
        result.ctx_size = is_embedding ? EMBEDDING_CTX_SIZE : AUTO_CTX_FALLBACK;
        return result;
    }

    result.ctx_size = compute_auto_context_size(model_info, available_memory_gb, is_embedding);
    return result;
}

} // namespace kv_cache_quant_detail

/// Resolve both the effective KV cache quant tier and ctx_size together
/// (KTD3). Called once per load (after residency-capacity eviction, so freed
/// memory is visible) and once per options-endpoint read.
///
/// `available_memory_gb` is queried once by the caller (mirroring
/// resolve_auto_ctx_size's pattern) rather than inside this function, so the
/// whole resolver stays a pure computation over its inputs and is directly
/// unit-testable with fabricated memory figures. `normalized_backend` and
/// `safety_table` are likewise supplied by the caller, which already links
/// the server core and has already normalized the backend name (KTD4) —
/// this function stays free of backend includes and reaches for no global
/// state, so it links into a standalone test.
///
/// Throws std::invalid_argument if any of the four kv-cache-quant option
/// values is out of its accepted set (R1/R2/R3), before any memory query —
/// the same exception type RuntimeConfig::validate_backend_choice uses.
inline KvCacheResolution resolve_kv_cache(const RecipeOptions& effective_options,
                                          const ModelInfo& model_info,
                                          double available_memory_gb,
                                          const std::string& normalized_backend,
                                          const KvCacheQuantSafetyTable& safety_table) {
    using namespace kv_cache_quant_detail;

    const std::string kv_quant_raw = kv_cache_option_string(effective_options, "kv_cache_quantization", "f16");
    auto kv_quant = parse_kv_cache_quant_config(kv_quant_raw);
    if (!kv_quant) {
        throw std::invalid_argument(
            "kv_cache_quantization must be one of: f16, auto, q8_0, q4_0 (got '" + kv_quant_raw + "')");
    }

    const std::string max_kv_raw = kv_cache_option_string(effective_options, "max_kv_quantization", "f16");
    auto max_kv_tier = parse_kv_cache_quant_tier(max_kv_raw);
    if (!max_kv_tier) {
        throw std::invalid_argument(
            "max_kv_quantization must be one of: f16, q8_0, q4_0 (got '" + max_kv_raw + "')");
    }

    const std::string min_kv_raw = kv_cache_option_string(effective_options, "min_kv_quantization", "q8_0");
    auto min_kv_tier = parse_kv_cache_quant_tier(min_kv_raw);
    if (!min_kv_tier) {
        throw std::invalid_argument(
            "min_kv_quantization must be one of: f16, q8_0, q4_0 (got '" + min_kv_raw + "')");
    }

    const std::string priority_raw = kv_cache_option_string(effective_options, "kv_cache_priority", "balanced");
    auto priority = parse_kv_cache_priority(priority_raw);
    if (!priority) {
        throw std::invalid_argument(
            "kv_cache_priority must be one of: max_context, balanced, max_speed (got '" + priority_raw + "')");
    }

    if (*kv_quant == KvCacheQuantConfig::F16) {
        return resolve_kv_cache_f16_only(effective_options, model_info, available_memory_gb, /*structurally_ineligible=*/false);
    }
    if (*kv_quant == KvCacheQuantConfig::Auto && *priority == KvCachePriority::MaxSpeed) {
        // R3: max_speed disables auto-quantization entirely; context
        // auto-sizing stays at f16 only. Does not consult min_kv_quantization.
        return resolve_kv_cache_f16_only(effective_options, model_info, available_memory_gb, /*structurally_ineligible=*/false);
    }

    // --- Build the candidate ladder ---
    std::vector<KvCacheQuantTier> ladder;
    if (*kv_quant == KvCacheQuantConfig::Auto) {
        KvCacheQuantTier floor = *min_kv_tier;
        if (*priority == KvCachePriority::Balanced) {
            // Invariant, not a default: floors at the higher quality of q8_0
            // and min_kv_quantization. No min_kv_quantization value pulls
            // balanced down to q4_0.
            floor = kv_cache_quant_tier_higher_quality(KvCacheQuantTier::Q8_0, *min_kv_tier)
                  ? KvCacheQuantTier::Q8_0 : *min_kv_tier;
        }
        // max_context floors at min_kv_quantization unconditionally (floor already set above).
        const int lo = kv_cache_quant_tier_rank(*max_kv_tier);
        const int hi = kv_cache_quant_tier_rank(floor);
        for (int r = lo; r <= hi; ++r) {
            ladder.push_back(kv_cache_quant_tier_from_rank(r));
        }
    } else {
        // Explicit q8_0/q4_0: no ladder-walking, subject to the same gates (R1).
        ladder.push_back(*kv_cache_quant_tier_from_config(*kv_quant));
    }

    // --- Filter through both eligibility gates (R5, R9) ---
    const int64_t key_dim = model_info.gguf.key_length;
    const int64_t value_dim = model_info.gguf.value_length;
    std::vector<KvCacheQuantTier> eligible;
    bool ladder_had_quant_candidate = false;
    bool quant_candidate_survived = false;
    std::string skip_trace;
    for (auto tier : ladder) {
        if (tier != KvCacheQuantTier::F16) ladder_had_quant_candidate = true;
        if (!kv_cache_quant_backend_eligible(tier, normalized_backend, safety_table)) {
            skip_trace += kv_cache_quant_tier_to_string(tier) + "=skipped(backend gate) ";
            continue;
        }
        if (!kv_cache_quant_model_eligible(tier, key_dim, value_dim)) {
            skip_trace += kv_cache_quant_tier_to_string(tier) + "=skipped(model gate) ";
            continue;
        }
        if (tier != KvCacheQuantTier::F16) quant_candidate_survived = true;
        eligible.push_back(tier);
    }
    // R14: the ladder wanted quantization but nothing below f16 survived the
    // gates — structural, not a fit shortfall. f16 itself always passes both
    // gates, so it surviving (and even being selected) does not make this
    // false; the point of R14 is that no *quantization* was possible.
    const bool structurally_ineligible = ladder_had_quant_candidate && !quant_candidate_survived;

    if (eligible.empty()) {
        LOG(DEBUG, "AutoTune") << "resolve_kv_cache: " << model_info.model_name
                               << " — backend=" << normalized_backend << " " << skip_trace
                               << "-> f16 (no eligible tier)" << " ";
        return resolve_kv_cache_f16_only(effective_options, model_info, available_memory_gb, structurally_ineligible);
    }

    bool is_embedding = (model_info.type == ModelType::EMBEDDING);
    if (available_memory_gb <= 0) {
        // Undetectable memory falls back to the existing fallback context at
        // f16, without consulting the ladder.
        return resolve_kv_cache_f16_only(effective_options, model_info, available_memory_gb, /*structurally_ineligible=*/false);
    }

    json ctx_json = effective_options.get_option("ctx_size");
    const int64_t requested_ctx = ctx_json.is_number() ? ctx_json.get<int64_t>() : -1;
    const bool ctx_explicit = (requested_ctx != -1);
    const int64_t target = ctx_explicit
        ? requested_ctx
        : (model_info.max_context_window > 0 ? model_info.max_context_window : AUTO_CTX_UNKNOWN_MAX);

    // Walk highest-quality first; select the first eligible tier that
    // reaches the target. Track the lowest-quality survivor's achieved
    // context along the way — it is both R7's shrink target and R8's
    // "best tier tried" on exhaustion.
    KvCacheQuantTier chosen = eligible.back();
    int64_t chosen_achieved = 0;
    bool reached_target = false;
    for (auto tier : eligible) {
        const int64_t achieved = compute_auto_context_size(
            model_info, available_memory_gb, is_embedding, kv_cache_quant_bytes_per_element(tier));
        skip_trace += kv_cache_quant_tier_to_string(tier) + "=" + std::to_string(achieved) +
            (achieved >= target ? "(fits) " : "(short of target) ");
        if (tier == eligible.back()) chosen_achieved = achieved;
        if (!reached_target && achieved >= target) {
            chosen = tier;
            chosen_achieved = achieved;
            reached_target = true;
            break;
        }
    }

    KvCacheResolution result;
    if (reached_target) {
        result.tier = chosen;
        result.ctx_size = ctx_explicit ? requested_ctx : chosen_achieved;
        result.ctx_size_is_auto = !ctx_explicit;
        result.structurally_ineligible = structurally_ineligible;
        LOG(DEBUG, "AutoTune") << "resolve_kv_cache: " << model_info.model_name
                               << " — backend=" << normalized_backend << " target=" << target
                               << " " << skip_trace << "-> " << kv_cache_quant_tier_to_string(chosen)
                               << " ";
    } else if (ctx_explicit) {
        // R8: an explicit request no eligible tier can fit. Raised at
        // resolve time, before a backend server is constructed (KTD13).
        result.failure = "Requested ctx_size " + std::to_string(requested_ctx) +
            " does not fit at any eligible KV cache quant tier for this model; the best tier "
            "tried (" + kv_cache_quant_tier_to_string(eligible.back()) + ") supports at most " +
            std::to_string(chosen_achieved) + " tokens.";
        LOG(DEBUG, "AutoTune") << "resolve_kv_cache: " << model_info.model_name
                               << " — backend=" << normalized_backend << " target=" << target
                               << " " << skip_trace << "-> R8 failure" << " ";
        return result;
    } else {
        // R7: shrink to the lowest-quality surviving tier's maximum — the
        // same memory-constrained behavior ctx_size: -1 has today.
        result.tier = eligible.back();
        result.ctx_size = chosen_achieved;
        result.ctx_size_is_auto = true;
        result.structurally_ineligible = structurally_ineligible;
        LOG(DEBUG, "AutoTune") << "resolve_kv_cache: " << model_info.model_name
                               << " — backend=" << normalized_backend << " target=" << target
                               << " " << skip_trace << "-> " << kv_cache_quant_tier_to_string(result.tier)
                               << " (shrunk, R7)" << " ";
    }

    if (result.tier != KvCacheQuantTier::F16) {
        const std::string llamacpp_args =
            kv_cache_option_string(effective_options, "llamacpp_args", "");
        if (llamacpp_args_disable_flash_attention(llamacpp_args)) {
            // R11: raised at resolve time, once a tier below f16 is chosen,
            // before a backend server is constructed (KTD13).
            KvCacheResolution conflict;
            conflict.failure = "kv_cache_quantization resolved to " +
                kv_cache_quant_tier_to_string(result.tier) +
                ", which requires flash attention, but llamacpp_args explicitly disables it "
                "(-fa/--flash-attn off). Remove the override or use kv_cache_quantization: f16.";
            return conflict;
        }
    }

    return result;
}


} // namespace lemon
