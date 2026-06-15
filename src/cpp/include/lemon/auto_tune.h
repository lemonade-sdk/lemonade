#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <lemon/model_manager.h>
#include <lemon/system_info.h>
#include <lemon/system_metrics_platform.h>
#include <lemon/utils/aixlog.hpp>

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
static double estimate_kv_bytes_per_token_from_model_size(double model_size_gb) {
    // Layer count scales with model size; 16 KV heads assumed uniformly.
    if (model_size_gb < 1.0) {
        // Tiny model (< 1B)
        return 128.0 * 1024.0;  // 128 KB/token (12 layers)
    } else if (model_size_gb < 3.0) {
        // ~3B class
        return 224.0 * 1024.0;  // 224 KB/token (28 layers)
    } else if (model_size_gb < 8.0) {
        // ~7B class
        return 256.0 * 1024.0;  // 256 KB/token (32 layers)
    } else if (model_size_gb < 16.0) {
        // ~14B class
        return 320.0 * 1024.0;  // 320 KB/token (40 layers)
    } else if (model_size_gb < 32.0) {
        // ~32B class
        return 512.0 * 1024.0; // 512 KB/token (64 layers)
    } else if (model_size_gb < 64.0) {
        // ~70B class
        return 640.0 * 1024.0; // 640 KB/token (80 layers)
    } else {
        // 100B+
        return 768.0 * 1024.0; // 768 KB/token (96 layers)
    }
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
            double available = std::max(0.0, total_gb - used_gb);
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
                double available = std::max(0.0, gpu.vram_gb - used_gb);
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
                double available = std::max(0.0, gpu.vram_gb - used_gb);
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
            double free_unified = std::max(0.0, apple.virtual_gb - used_gb);
            double available = std::min(apple.vram_gb, free_unified);
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
        double available = std::max(0.0, apple.virtual_gb - used_gb);
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
        double available = std::max(0.0, total_gb - used_gb);
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
            double available = std::max(0.0, gpu.virtual_gb - used_gb);
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

/**
 * Compute the maximum context window that fits in available memory based on
 * GGUF architecture metadata.
 *
 * Formula (from auto-tune schema):
 *   kv_bytes_per_token = block_count × head_count_kv × key_length × 2[F16] × 2[K+V]
 *   max_ctx = floor((available_memory_gb × 1024³ - weights) / kv_bytes_per_token)
 *   ctx_size = min(model_max_context_window, max_ctx)
 *
 * When GGUF metadata is unavailable, estimates kv_bytes_per_token from model size.
 * For embedding models, the result is floored at EMBEDDING_CTX_SIZE.
 *
 * @param model_info       Model metadata including GGUF architecture fields
 * @param available_memory_gb  Total memory available for the model (VRAM or system RAM)
 * @param is_embedding     Whether this is an embedding model (applies minimum floor)
 * @return Resolved context size, or AUTO_CTX_FALLBACK if computation fails
 */
inline int64_t compute_auto_context_size(const ModelInfo& model_info,
                                          double available_memory_gb,
                                          bool is_embedding = false) {
    if (available_memory_gb <= 0) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — not enough memory, returning " << AUTO_CTX_FALLBACK  << " ";
        return AUTO_CTX_FALLBACK;
    }

    // KV cache bytes per token
    double kv_bytes_per_token = 0;
    bool estimated = false;

    // Try exact GGUF metadata first
    int64_t block_count = model_info.gguf_block_count;
    int64_t head_count_kv = model_info.gguf_head_count_kv;
    int64_t key_length = model_info.gguf_key_length;

    if (block_count > 0 && head_count_kv > 0 && key_length > 0) {
        // Exact formula: layers × kv_heads × head_dim × 2[F16] × 2[K+V]
        kv_bytes_per_token = static_cast<double>(block_count)
                            * static_cast<double>(head_count_kv)
                            * static_cast<double>(key_length)
                            * 2.0  // F16 = 2 bytes per element
                            * 2.0; // K cache + V cache
    } else {
        // GGUF metadata missing — estimate from model size
        kv_bytes_per_token = estimate_kv_bytes_per_token_from_model_size(model_info.size);
        estimated = true;
    }

    if (kv_bytes_per_token <= 0) {
        return AUTO_CTX_FALLBACK;
    }

    // Available memory for KV cache = total - used - model weights
    // (used is already subtracted in get_available_memory_gb)
    double model_weight_gb = std::max(0.0, model_info.size);
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
                           << " — GGUF: blocks=" << block_count << ", kv_heads=" << head_count_kv
                           << ", key_len=" << key_length
                           << " | kv_cache=" << std::fixed << std::setprecision(2)
                           << (kv_bytes_per_token / (1024.0 * 1024.0)) << " MB/token"
                           << (estimated ? " (est)" : "")
                           << " | memory: " << available_memory_gb << " GB avail, "
                           << model_weight_gb << " GB weights → " << available_for_kv_gb << " GB for KV"
                           << " | ctx=" << max_ctx_from_memory << " → " << ctx_size
                           << clamp_note << " ";
    return ctx_size;
}

/// Auto-resolve ctx_size if it is -1 in the effective options.
/// Returns the resolved context size, or -2 if no auto-resolution is needed
/// (i.e. ctx_size was already set to an explicit non-negative value).
/// The caller should check the return value and update RecipeOptions accordingly.
inline int64_t resolve_auto_ctx_size(const RecipeOptions& effective_options,
                                      const ModelInfo& model_info) {
    json ctx_json = effective_options.get_option("ctx_size");
    int64_t ctx_size = ctx_json.is_number() ? ctx_json.get<int64_t>() : -1;

    if (ctx_size != -1) {
        return -2;  // Explicit value, no auto-resolution needed
    }

    bool is_embedding = (model_info.type == ModelType::EMBEDDING);
    double available_gb = get_available_memory_gb(model_info.device);

    if (available_gb <= 0) {
        int64_t fallback = is_embedding ? EMBEDDING_CTX_SIZE : AUTO_CTX_FALLBACK;
        LOG(DEBUG, "AutoTune") << "resolve_auto_ctx_size: " << model_info.model_name
                               << " — memory undetectable, returning " << fallback;
        return fallback;
    }

    int64_t result = compute_auto_context_size(model_info, available_gb, is_embedding);
    LOG(DEBUG, "AutoTune") << "resolve_auto_ctx_size: " << model_info.model_name
                           << " → ctx_size=" << result;
    return result;
}

} // namespace lemon
