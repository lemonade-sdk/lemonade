#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <gpu_device_memory.h>
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
inline double get_used_memory_gb(DeviceType device_type) {
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
///
/// The GPU total here comes from the first available card while the used figure is
/// machine-wide, so on a multi-GPU host the two can describe different cards. Prefer
/// get_available_memory_detail(), which scopes both to one device when it can.
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

inline GpuDeviceReading to_gpu_device_reading(const GPUInfo& gpu) {
    GpuDeviceReading reading;
    reading.available = gpu.available;
    reading.index = gpu.index;
    reading.vram_gb = gpu.vram_gb;
    reading.virtual_gb = gpu.virtual_gb;
    reading.vram_used_gb = gpu.vram_used_gb;
    reading.virtual_used_gb = gpu.virtual_used_gb;
    return reading;
}

inline std::vector<GpuDeviceReading> to_gpu_device_readings(const std::vector<GPUInfo>& gpus) {
    std::vector<GpuDeviceReading> readings;
    readings.reserve(gpus.size());
    for (const auto& gpu : gpus) readings.push_back(to_gpu_device_reading(gpu));
    return readings;
}

/// Outcome of scoping the memory query to the GPU a model is being placed on.
struct DeviceMemoryResult {
    double available_gb = 0.0;
    // False when the query fell back to the aggregate reading, which can misreport
    // headroom once a second model is resident on another GPU.
    bool per_device = false;
    // Set when a pool was picked without the caller naming a device.
    bool ambiguous = false;
    std::string device_label;
    // How many GPUs were reachable for the resolved vendor, regardless of whether any
    // of them yielded a usable per-device reading.
    int reachable_gpu_count = 0;
};

/// Available memory for a model placement, scoped to the specific GPU when possible.
///
/// When `backend`/`device_string` identify a GPU and the platform reports per-device
/// usage counters, both the capacity and the usage come from that one device.
/// Otherwise this falls back to get_available_memory_gb() and reports per_device=false
/// so the caller can say so.
inline DeviceMemoryResult get_available_memory_detail(DeviceType device_type,
                                                      const std::string& backend,
                                                      const std::string& device_string) {
    DeviceMemoryResult result;

    if (device_type & DEVICE_GPU) {
        auto si = create_system_info();
        const auto amd = amd_memory_candidates(to_gpu_device_reading(si->get_amd_igpu_device()),
                                               to_gpu_device_readings(si->get_amd_dgpu_devices()));
        const auto nvidia = nvidia_memory_candidates(to_gpu_device_readings(si->get_nvidia_gpu_devices()));
        result.reachable_gpu_count = count_reachable_gpus(amd, nvidia, backend, device_string);
        const GpuMemoryPool pool = select_gpu_memory_pool(amd, nvidia, backend, device_string);
        if (pool.resolved) {
            result.available_gb = pool.free_gb();
            result.per_device = true;
            result.ambiguous = pool.ambiguous;
            result.device_label = pool.label;
            LOG(DEBUG, "AutoTune") << "get_available_memory_detail: GPU (" << pool.label
                                   << ") total=" << std::fixed << std::setprecision(2)
                                   << pool.total_gb << " GB, used=" << pool.used_gb
                                   << " GB → " << result.available_gb << " GB available ";
            return result;
        }
    }

    result.available_gb = get_available_memory_gb(device_type);
    return result;
}

/// Resolve ctx_size from available memory and model metadata.
///
/// `out_is_memory_fallback`, if non-null, is set to true only when the returned value
/// is AUTO_CTX_FALLBACK *because there wasn't enough memory to do better* — as opposed
/// to 4096 being the model's own max_context_window, the computed value legitimately
/// landing on 4096, or kv_bytes_per_token being unavailable for non-memory reasons.
/// Callers that warn about "fell back due to low memory" must check this flag rather
/// than comparing the result to AUTO_CTX_FALLBACK, since 4096 is also a valid non-
/// fallback value.
inline int64_t compute_auto_context_size(const ModelInfo& model_info,
                                          double available_memory_gb,
                                          bool is_embedding = false,
                                          bool* out_is_memory_fallback = nullptr) {
    if (out_is_memory_fallback) *out_is_memory_fallback = false;

    if (available_memory_gb <= 0) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — not enough memory, returning " << AUTO_CTX_FALLBACK  << " ";
        if (out_is_memory_fallback) *out_is_memory_fallback = true;
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
            model_info.gguf, &kv_cache_scale);
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
        kv_bytes_per_token = estimate_kv_bytes_per_token_from_model_size(model_info.size);
        estimated = true;
    }

    if (kv_bytes_per_token <= 0) {
        // Not a memory shortage — the model's metadata just didn't yield a usable
        // per-token KV size.
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
        if (out_is_memory_fallback) *out_is_memory_fallback = true;
        return AUTO_CTX_FALLBACK;
    }

    double available_bytes = available_for_kv_gb * BYTES_PER_GIB;
    int64_t max_ctx_from_memory = static_cast<int64_t>(std::floor(available_bytes / kv_bytes_per_token));

    if (max_ctx_from_memory <= 0) {
        if (out_is_memory_fallback) *out_is_memory_fallback = true;
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

    // Scoping the headroom figure to one GPU needs a device selection to scope to.
    // Only llama.cpp exposes one today, so the new per-device path is limited to that
    // recipe for now; other recipes keep the pre-existing aggregate estimate rather
    // than silently changing what "available memory" means for them.
    const bool is_llamacpp = effective_options.get_recipe() == "llamacpp";
    json device_json = is_llamacpp ? effective_options.get_option("llamacpp_device") : json();
    json backend_json = is_llamacpp ? effective_options.get_option("llamacpp_backend") : json();
    const std::string device_string = device_json.is_string() ? device_json.get<std::string>() : "";
    const std::string backend = backend_json.is_string() ? backend_json.get<std::string>() : "";

    const DeviceMemoryResult memory = is_llamacpp
        ? get_available_memory_detail(model_info.device, backend, device_string)
        : DeviceMemoryResult{get_available_memory_gb(model_info.device), false, false, "", 0};
    const double available_gb = memory.available_gb;

    // Without a per-device reading the headroom figure mixes one card's capacity with
    // the machine's busiest-card usage, which understates free VRAM once a second
    // model is resident on another GPU. On a single-GPU host there is nothing else to
    // scope to, so this isn't actionable and shouldn't be reported.
    CtxMemoryScope scope;
    scope.is_gpu = (model_info.device & DEVICE_GPU) != 0;
    scope.per_device = memory.per_device;
    scope.ambiguous = memory.ambiguous;
    scope.device_named = !device_string.empty();
    scope.reachable_gpu_count = memory.reachable_gpu_count;
    const CtxScopeWarning warning = classify_ctx_scope(scope);

    if (warning == CtxScopeWarning::Unscoped) {
        LOG(WARNING, "AutoTune") << model_info.model_name << ": could not scope VRAM headroom to '"
                                 << device_string
                                 << "'; auto-tuned ctx_size is based on a machine-wide estimate "
                                    "and may be far smaller than this GPU can hold. "
                                    "Pass --ctx-size to set it explicitly."  << " ";
    } else if (warning == CtxScopeWarning::Ambiguous) {
        LOG(WARNING, "AutoTune") << model_info.model_name
                                 << ": no target GPU specified; auto-tuning ctx_size against the "
                                    "most constrained GPU (" << memory.device_label << "). "
                                 << (is_llamacpp ? "Pass --llamacpp-device or --ctx-size"
                                                 : "Pass --ctx-size")
                                 << " to override."  << " ";
    }

    if (available_gb <= 0) {
        int64_t fallback = is_embedding ? EMBEDDING_CTX_SIZE : AUTO_CTX_FALLBACK;
        LOG(WARNING, "AutoTune") << model_info.model_name
                                 << ": available memory could not be determined; falling back to "
                                    "ctx_size=" << fallback
                                 << ". Pass --ctx-size to set it explicitly."  << " ";
        return fallback;
    }

    bool is_memory_fallback = false;
    int64_t result = compute_auto_context_size(model_info, available_gb, is_embedding,
                                               &is_memory_fallback);
    LOG(DEBUG, "AutoTune") << "resolve_auto_ctx_size: " << model_info.model_name
                           << " → ctx_size=" << result;

    if (!is_embedding && is_memory_fallback) {
        LOG(WARNING, "AutoTune") << model_info.model_name << ": only " << std::fixed
                                 << std::setprecision(2) << available_gb << " GB free on "
                                 << (memory.device_label.empty() ? "the target device"
                                                                 : memory.device_label)
                                 << " — not enough for the model weights plus a larger KV cache, "
                                    "so ctx_size fell back to " << AUTO_CTX_FALLBACK
                                 << ". Free VRAM or pass --ctx-size to override."  << " ";
    }
    return result;
}

} // namespace lemon
