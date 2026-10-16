#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <system_error>
#include <lemon/auto_context_budget.h>
#include <lemon/gpu_memory_selection.h>
#include <lemon/model_manager.h>
#include <lemon/system_info.h>
#include <lemon/system_metrics_platform.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/path_utils.h>

namespace lemon {

// Minimum context size for embedding models (applied as a floor after auto-resolution)
constexpr int64_t EMBEDDING_CTX_SIZE = 8192;

// Fallback context size when auto-resolution cannot be computed
constexpr int64_t AUTO_CTX_FALLBACK = 4096;
constexpr int64_t AUTO_CTX_NO_CAPACITY = 0;

// Hard cap on context size when the model's max context window is unknown.
// No modern model supports less than this, and it prevents runaway allocation.
constexpr int64_t AUTO_CTX_UNKNOWN_MAX = 32768;

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
inline double get_available_memory_gb(DeviceType device_type,
                                      GpuMemoryVendor gpu_vendor = GpuMemoryVendor::Any,
                                      const std::string& gpu_device = "",
                                      bool* unified_gpu_out = nullptr,
                                      bool* memory_known_out = nullptr) {
    if (unified_gpu_out) *unified_gpu_out = false;
    if (memory_known_out) *memory_known_out = false;
    auto si = create_system_info();

    // GPU recipes: use VRAM
    if (device_type & DEVICE_GPU) {
        auto amd_igpu = si->get_amd_igpu_device();
        auto amd_dgpus = si->get_amd_dgpu_devices();
        auto nvidia_gpus = si->get_nvidia_gpu_devices();
        auto apple_gpu = si->get_apple_silicon_device();
        const char* cuda_visible_devices_env = std::getenv("CUDA_VISIBLE_DEVICES");
        const std::optional<std::string> cuda_visible_devices =
            cuda_visible_devices_env
                ? std::optional<std::string>(cuda_visible_devices_env)
                : std::nullopt;
        auto pool = select_gpu_memory_pool(gpu_vendor,
                                           amd_igpu, amd_dgpus, nvidia_gpus, apple_gpu,
                                           gpu_device,
                                           cuda_visible_devices);
        if (pool.total_gb > 0) {
            if (unified_gpu_out) *unified_gpu_out = pool.unified;
            double gpu_used_gb = pool.used_gb;
            if (!pool.unified &&
                (!std::isfinite(gpu_used_gb) || gpu_used_gb < 0.0)) {
                auto metrics = create_metrics_platform();
                if (metrics) gpu_used_gb = metrics->get_vram_usage_gb();
            }
            const double physical_total_gb = pool.physical_total_gb;
            const double ram_used_gb = pool.unified
                ? get_used_memory_gb(DEVICE_CPU) : -1.0;
            const auto available_gpu_gb = auto_context_available_gpu_gib(
                pool.total_gb, gpu_used_gb, pool.unified,
                physical_total_gb, ram_used_gb);
            if (!available_gpu_gb) {
                LOG(DEBUG, "AutoTune")
                    << "get_available_memory_gb: no reliable GPU/RAM usage; using context fallback";
                return 0.0;
            }
            if (memory_known_out) *memory_known_out = true;
            const double available = *available_gpu_gb;
            LOG(DEBUG, "AutoTune") << "get_available_memory_gb: GPU (" << pool.label
                                   << ") total=" << std::fixed << std::setprecision(2)
                                   << pool.total_gb << " GB, used=" << gpu_used_gb
                                   << " GB → " << available << " GB available" << " ";
            return available;
        }

        if (gpu_vendor != GpuMemoryVendor::Any) {
            LOG(DEBUG, "AutoTune") << "get_available_memory_gb: selected GPU vendor unavailable";
            return 0.0;
        }

    }

    // CPU / NPU: use system RAM
    const double used_gb = get_used_memory_gb(device_type);
    // Apple Silicon: the full unified-memory pool (total physical RAM) is reported
    // as virtual_gb on the Apple Silicon device.
    auto apple = si->get_apple_silicon_device();
    if (apple.available && apple.virtual_gb > 0) {
        if (memory_known_out) *memory_known_out = true;
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
        if (memory_known_out) *memory_known_out = true;
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
            if (memory_known_out) *memory_known_out = true;
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
                                          bool unified_gpu = false) {
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
        const bool encoder_model = model_info.type == ModelType::EMBEDDING ||
                                   model_info.type == ModelType::RERANKING;
        kv_bytes_per_token = estimate_bert_encoder_kv_bytes_per_token(
            model_info.gguf, encoder_model);
        if (kv_bytes_per_token <= 0.0) {
            kv_bytes_per_token = estimate_kv_bytes_per_token_from_model_size(model_info.size);
        }
        estimated = true;
    }

    if (kv_bytes_per_token <= 0) {
        return AUTO_CTX_FALLBACK;
    }

    // Include all known model artifacts, not just the registry-reported size.
    // The mmproj and speculative draft files are separately loaded and can be
    // large. Local file_size also covers stale/rounded registry model sizes.
    auto file_gib = [](const std::string& path) -> double {
        if (path.empty()) return 0.0;
        std::error_code ec;
        auto bytes = std::filesystem::file_size(utils::path_from_utf8(path), ec);
        return ec ? 0.0 : static_cast<double>(bytes) / BYTES_PER_GIB;
    };
    const std::string mmproj_path = model_info.resolved_path("mmproj");
    const std::string draft_path = model_info.resolved_path("draft");
    // refresh_on_disk_size() includes every resolved artifact in info.size.
    // The separately resolved files must NOT be charged a second time.
    const double model_weight_gb = auto_context_total_weights_gib(
        model_info.size, file_gib(model_info.resolved_path()),
        file_gib(mmproj_path), file_gib(draft_path));
    if (model_weight_gb <= 0.0) return AUTO_CTX_FALLBACK;

    const bool uses_mtp = std::find(model_info.labels.begin(), model_info.labels.end(),
                                    "mtp") != model_info.labels.end();
    const bool speculative = uses_mtp || !model_info.checkpoint("draft").empty();
    if (uses_mtp) {
        kv_bytes_per_token *= AUTO_CTX_MTP_KV_MULTIPLIER;
    }
    if (!draft_path.empty()) {
        GgufMetadata draft_metadata;
        if (read_gguf_metadata(draft_metadata, draft_path)) {
            const double draft_kv = compute_weighted_kv_cache_bytes_per_token(draft_metadata);
            if (draft_kv > 0.0) kv_bytes_per_token += draft_kv;
        } else {
            kv_bytes_per_token *= AUTO_CTX_UNKNOWN_DRAFT_KV_MULTIPLIER;
        }
    }

    const auto budget = estimate_auto_context_budget(
        available_memory_gb, model_weight_gb,
        kv_bytes_per_token, (model_info.device & DEVICE_GPU) != 0,
        speculative, unified_gpu);
    const int64_t max_ctx_from_memory = budget.max_tokens;
    if (max_ctx_from_memory <= 0) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — model and runtime reserves exceed available memory ("
                               << std::fixed << std::setprecision(2) << available_memory_gb
                               << " GiB), cannot fit automatic context";
        return AUTO_CTX_NO_CAPACITY;
    }

    const int64_t runtime_minimum = is_embedding ? EMBEDDING_CTX_SIZE : 0;
    const int64_t ctx_size = auto_context_clamp_tokens(
        max_ctx_from_memory, model_info.max_context_window,
        AUTO_CTX_UNKNOWN_MAX, runtime_minimum);
    if (ctx_size == AUTO_CTX_NO_CAPACITY) {
        LOG(DEBUG, "AutoTune") << "compute_auto_context_size: " << model_info.model_name
                               << " — budget fits " << max_ctx_from_memory
                               << " tokens, runtime requires " << runtime_minimum;
        return AUTO_CTX_NO_CAPACITY;
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
                           << " | memory: " << available_memory_gb << " GiB free, "
                           << budget.model_gib << " GiB model, "
                           << budget.reserve_gib << " GiB runtime reserve, "
                           << budget.kv_gib << " GiB for KV"
                           << " | ctx=" << max_ctx_from_memory << " → " << ctx_size << " ";
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
    std::string backend;
    std::string device;
    const std::string recipe = effective_options.get_recipe();
    const json backend_json = effective_options.get_option(recipe + "_backend");
    const json device_json = effective_options.get_option(recipe + "_device");
    if (backend_json.is_string()) backend = backend_json.get<std::string>();
    if (device_json.is_string()) device = device_json.get<std::string>();
    bool ctx_unified_gpu = false;
    bool memory_known = false;
    double available_gb = get_available_memory_gb(
        model_info.device, gpu_memory_vendor_for_target(backend, device), device,
        &ctx_unified_gpu, &memory_known);

    if (available_gb <= 0) {
        if (memory_known) return AUTO_CTX_NO_CAPACITY;
        int64_t fallback = is_embedding ? EMBEDDING_CTX_SIZE : AUTO_CTX_FALLBACK;
        LOG(DEBUG, "AutoTune") << "resolve_auto_ctx_size: " << model_info.model_name
                               << " — memory undetectable, returning " << fallback;
        return fallback;
    }

    int64_t result = compute_auto_context_size(model_info, available_gb,
                                               is_embedding, ctx_unified_gpu);
    LOG(DEBUG, "AutoTune") << "resolve_auto_ctx_size: " << model_info.model_name
                           << " → ctx_size=" << result;
    return result;
}

} // namespace lemon
