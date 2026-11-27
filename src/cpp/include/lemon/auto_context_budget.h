#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include <lemon/utils/custom_args.h>

namespace lemon {

inline double auto_context_total_weights_gib(double registry_gib,
                                              double main_gib,
                                              double mmproj_gib,
                                              double draft_gib) {
    auto positive = [](double value) {
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    };
    return (std::max)(positive(registry_gib), positive(main_gib) +
                     positive(mmproj_gib) + positive(draft_gib));
}

inline int64_t auto_context_clamp_tokens(int64_t memory_max,
                                          int64_t model_max,
                                          int64_t unknown_max,
                                          int64_t runtime_minimum = 0) {
    if (memory_max <= 0 || runtime_minimum < 0 || memory_max < runtime_minimum)
        return 0;
    const int64_t metadata_limit = model_max > 0 ? model_max : unknown_max;
    if (metadata_limit <= 0) return 0;
    return (std::min)(memory_max, (std::max)(metadata_limit, runtime_minimum));
}

struct AutoContextBudget {
    double model_gib = 0.0;
    double reserve_gib = 0.0;
    double kv_gib = 0.0;
    int64_t max_tokens = 0;
};

struct AutoContextReserveRange {
    double fraction;
    double minimum_gib;
    double maximum_gib;

    double estimate(double base_gib) const {
        return std::clamp(base_gib * fraction, minimum_gib, maximum_gib);
    }
};

struct AutoContextReservePolicy {
    AutoContextReserveRange memory_headroom;
    AutoContextReserveRange compute_workspace;
};

// Dedicated VRAM needs only a fixed 1 GiB allocation headroom; runtime
// workspace is reserved separately. UMA and host RAM retain larger reserves
// to protect the OS and other processes. Not additional model weights.
inline constexpr AutoContextReservePolicy AUTO_CTX_DISCRETE_GPU_RESERVE{
    {0.0, 1.0, 1.0}, {0.05, 0.5, 1.75}};
inline constexpr AutoContextReservePolicy AUTO_CTX_UNIFIED_GPU_RESERVE{
    {0.08, 0.75, 10.0}, {0.04, 0.5, 3.0}};
inline constexpr AutoContextReservePolicy AUTO_CTX_HOST_RESERVE{
    {0.05, 1.0, std::numeric_limits<double>::max()},
    {0.05, 0.5, std::numeric_limits<double>::max()}};
inline constexpr double AUTO_CTX_SPECULATIVE_WORKSPACE_GIB = 0.25;
inline constexpr double AUTO_CTX_MTP_KV_MULTIPLIER = 1.07;
inline constexpr double AUTO_CTX_UNKNOWN_DRAFT_KV_MULTIPLIER = 1.25;

inline double auto_context_cap_shared_memory_gib(double gpu_free_gib,
                                                 double physical_total_gib,
                                                 double physical_used_gib) {
    if (physical_total_gib <= 0.0 || physical_used_gib < 0.0 ||
        !std::isfinite(physical_total_gib) || !std::isfinite(physical_used_gib)) {
        return gpu_free_gib;
    }
    return (std::min)(gpu_free_gib,
                      (std::max)(0.0, physical_total_gib - physical_used_gib));
}

inline std::optional<double> auto_context_available_gpu_gib(
    double gpu_total_gib, double gpu_used_gib, bool unified,
    double physical_total_gib, double physical_used_gib) {
    if (!std::isfinite(gpu_total_gib) || gpu_total_gib <= 0.0)
        return std::nullopt;

    const bool gpu_usage_known = std::isfinite(gpu_used_gib) && gpu_used_gib >= 0.0;
    const bool ram_usage_known = std::isfinite(physical_total_gib) &&
        physical_total_gib > 0.0 && std::isfinite(physical_used_gib) &&
        physical_used_gib > 0.0;

    // Shared-memory GPUs also need RAM usage to account for other processes.
    if ((unified && !ram_usage_known) || (!unified && !gpu_usage_known))
        return std::nullopt;

    double available = gpu_usage_known
        ? (std::max)(0.0, gpu_total_gib - gpu_used_gib) : gpu_total_gib;
    if (unified && ram_usage_known)
        available = auto_context_cap_shared_memory_gib(
            available, physical_total_gib, physical_used_gib);
    return available;
}

// llama.cpp's Vulkan backend allocates in blocks of up to 1 GiB, and a block
// cannot straddle the dedicated and shared heaps.
inline constexpr double AUTO_CTX_HEAP_SPLIT_LOSS_GIB = 1.0;

// Scaled by the total carve-out, not the free part, so that the reserve stays
// when another model fills the carve-out. It adds to the RAM headroom on purpose.
inline constexpr AutoContextReserveRange AUTO_CTX_CARVEOUT_GPU_HEADROOM{
    1.0 / 32.0, 0.0, 2.0};

struct AutoContextSplitGpuMemory {
    double available_gib = 0.0;  // allocatable across both heaps, minus headroom
    double carveout_gib = 0.0;   // the part of it that costs no OS-visible RAM
};

// UMA GPU with a firmware carve-out (AMD VGM): the carve-out is outside the
// OS-visible RAM, so only the shared part competes with the CPU for free RAM.
// Returns nullopt when a usage reading is unknown, or when the carve-out cannot
// hold one heap block (the firmware default), so the plain UMA budget applies.
inline std::optional<AutoContextSplitGpuMemory> auto_context_available_split_gpu_gib(
    double dedicated_total_gib, double dedicated_used_gib,
    double shared_total_gib, double shared_used_gib,
    double physical_total_gib, double physical_used_gib) {
    auto known = [](double value) { return std::isfinite(value) && value >= 0.0; };
    if (!known(dedicated_total_gib) || dedicated_total_gib <= AUTO_CTX_HEAP_SPLIT_LOSS_GIB ||
        !known(dedicated_used_gib) || !known(shared_total_gib) || !known(shared_used_gib))
        return std::nullopt;
    if (!std::isfinite(physical_total_gib) || physical_total_gib <= 0.0 ||
        !std::isfinite(physical_used_gib) || physical_used_gib <= 0.0)
        return std::nullopt;

    const double dedicated_free = (std::max)(0.0, dedicated_total_gib - dedicated_used_gib);
    const double shared_free = auto_context_cap_shared_memory_gib(
        (std::max)(0.0, shared_total_gib - shared_used_gib),
        physical_total_gib, physical_used_gib);
    // A block that does not fit in the carve-out's tail lands in shared memory.
    const double split_loss =
        (std::min)({AUTO_CTX_HEAP_SPLIT_LOSS_GIB, dedicated_free, shared_free});
    const double carveout = dedicated_free - split_loss;
    const double gpu_headroom =
        AUTO_CTX_CARVEOUT_GPU_HEADROOM.estimate(dedicated_total_gib);
    return AutoContextSplitGpuMemory{
        (std::max)(0.0, carveout + shared_free - gpu_headroom), carveout};
}

inline AutoContextBudget estimate_auto_context_budget(double free_gib,
                                                       double weights_gib,
                                                       double kv_bytes_per_token,
                                                       bool gpu,
                                                       bool speculative,
                                                       bool unified_gpu = false,
                                                       double physical_free_gib = -1.0) {
    AutoContextBudget result;
    if (!std::isfinite(free_gib) || !std::isfinite(weights_gib) ||
        !std::isfinite(kv_bytes_per_token) ||
        free_gib <= 0.0 || weights_gib <= 0.0 || kv_bytes_per_token <= 0.0) {
        return result;
    }

    result.model_gib = weights_gib;
    const AutoContextReservePolicy& reserve_policy = !gpu ? AUTO_CTX_HOST_RESERVE
        : unified_gpu ? AUTO_CTX_UNIFIED_GPU_RESERVE : AUTO_CTX_DISCRETE_GPU_RESERVE;
    const bool separate_ram_budget = gpu && unified_gpu &&
        std::isfinite(physical_free_gib) && physical_free_gib >= 0.0;
    const double memory_headroom_gib = reserve_policy.memory_headroom.estimate(
        separate_ram_budget ? physical_free_gib : free_gib);
    const double compute_workspace_gib =
        reserve_policy.compute_workspace.estimate(result.model_gib);
    const double speculative_workspace_gib =
        speculative ? AUTO_CTX_SPECULATIVE_WORKSPACE_GIB : 0.0;
    result.reserve_gib = memory_headroom_gib + compute_workspace_gib +
        speculative_workspace_gib;
    if (separate_ram_budget) {
        const double gpu_kv = free_gib - result.model_gib -
                              compute_workspace_gib - speculative_workspace_gib;
        const double ram_kv = physical_free_gib - result.model_gib -
                              result.reserve_gib;
        result.kv_gib = (std::max)(0.0, (std::min)(gpu_kv, ram_kv));
    } else {
        result.kv_gib = (std::max)(0.0, free_gib - result.model_gib - result.reserve_gib);
    }

    constexpr double bytes_per_gib = 1024.0 * 1024.0 * 1024.0;
    const double tokens = std::floor(result.kv_gib * bytes_per_gib / kv_bytes_per_token);
    if (tokens > 0.0) {
        const int64_t limit = std::numeric_limits<int64_t>::max();
        result.max_tokens = tokens >= static_cast<double>(limit)
            ? limit : static_cast<int64_t>(tokens);
    }
    return result;
}

// Byte sizes of llama.cpp's supported KV tensor dtypes (ggml, 32-value blocks).
// These are per scalar, not the model weight quantization. Unknown types are
// charged as F32 rather than risking an underestimate.
inline double auto_context_llamacpp_kv_element_bytes(const std::string& type) {
    if (type == "f16" || type == "bf16") return 2.0;
    if (type == "f32") return 4.0;
    if (type == "f8_e4m3") return 1.0;
    if (type == "q8_0") return 34.0 / 32.0;
    if (type == "q5_1") return 24.0 / 32.0;
    if (type == "q5_0") return 22.0 / 32.0;
    if (type == "q4_1") return 20.0 / 32.0;
    if (type == "q4_0" || type == "iq4_nl") return 18.0 / 32.0;
    return 4.0;
}

// Parse effective llama-server -ctk/-ctv settings, including env defaults.
// CLI flags override env, as in llama.cpp. Returning 1 keeps existing F16
// behavior and all non-llama.cpp recipes unchanged.
inline double auto_context_llamacpp_kv_bytes_ratio(
    const std::string& recipe, const std::string& custom_args,
    const char* key_env = std::getenv("LLAMA_ARG_CACHE_TYPE_K"),
    const char* value_env = std::getenv("LLAMA_ARG_CACHE_TYPE_V")) {
    if (recipe != "llamacpp") return 1.0;
    std::string key_type = key_env ? key_env : "f16";
    std::string value_type = value_env ? value_env : "f16";
    const auto args = utils::parse_custom_args(custom_args);
    for (size_t i = 0; i < args.size(); ++i) {
        const auto eq = args[i].find('=');
        const std::string flag = args[i].substr(0, eq);
        const bool key_flag = flag == "-ctk" || flag == "--cache-type-k";
        const bool value_flag = flag == "-ctv" || flag == "--cache-type-v";
        if (!key_flag && !value_flag) continue;
        const std::string type = eq != std::string::npos
            ? args[i].substr(eq + 1) : (i + 1 < args.size() ? args[++i] : "");
        if (key_flag) key_type = type;
        else value_type = type;
    }
    // The original F16 estimate assumes equally sized K and V. Taking the
    // larger format prevents undercharging V padding with asymmetric formats.
    // Equal formats, including Q8_0/Q8_0, keep their exact size ratio.
    return (std::max)(auto_context_llamacpp_kv_element_bytes(key_type),
                      auto_context_llamacpp_kv_element_bytes(value_type)) / 2.0;
}

// Whether llama-server keeps weights and KV on the GPU. Placement flags whose
// host share cannot be sized here count as partial.
inline bool auto_context_llamacpp_full_offload(
    const std::string& recipe, const std::string& custom_args, int64_t block_count,
    const char* gpu_layers_env = std::getenv("LLAMA_ARG_N_GPU_LAYERS")) {
    if (recipe != "llamacpp") return true;
    for (const char* name : {"LLAMA_ARG_CPU_MOE", "LLAMA_ARG_KV_OFFLOAD",
                             "LLAMA_ARG_N_CPU_FFN", "LLAMA_ARG_N_CPU_MOE",
                             "LLAMA_ARG_NO_KV_OFFLOAD", "LLAMA_ARG_OVERRIDE_TENSOR"}) {
        if (std::getenv(name)) return false;
    }

    std::string gpu_layers = gpu_layers_env ? gpu_layers_env : "auto";
    const auto args = utils::parse_custom_args(custom_args);
    for (size_t i = 0; i < args.size(); ++i) {
        const auto eq = args[i].find('=');
        const std::string flag = args[i].substr(0, eq);
        if (flag == "-cmoe" || flag == "--cpu-moe" ||
            flag == "-ncmoe" || flag == "--n-cpu-moe" ||
            flag == "-ncffn" || flag == "--n-cpu-ffn" ||
            flag == "-ot" || flag == "--override-tensor" ||
            flag == "-nkvo" || flag == "--no-kv-offload") return false;
        if (flag == "-ngl" || flag == "--gpu-layers" || flag == "--n-gpu-layers") {
            gpu_layers = eq != std::string::npos
                ? args[i].substr(eq + 1) : (i + 1 < args.size() ? args[++i] : "");
        }
    }

    if (gpu_layers == "auto" || gpu_layers == "all") return true;
    try {
        size_t consumed = 0;
        const int64_t layers = std::stoll(gpu_layers, &consumed);
        if (consumed != gpu_layers.size()) return false;
        return layers < 0 || (block_count > 0 && layers >= block_count);
    } catch (...) {
        return false;
    }
}

// Overflow-safe conversion for backend-specific resident-working-set budgets.
inline int64_t auto_context_tokens_from_kv_gib(double kv_gib, double kv_bytes_per_token) {
    if (!std::isfinite(kv_gib) || !std::isfinite(kv_bytes_per_token) ||
        kv_gib <= 0.0 || kv_bytes_per_token <= 0.0) return 0;
    const double tokens = std::floor(kv_gib * 1024.0 * 1024.0 * 1024.0 /
                                     kv_bytes_per_token);
    if (tokens >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return tokens > 0.0 ? static_cast<int64_t>(tokens) : 0;
}

} // namespace lemon
