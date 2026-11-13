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

// Conservative runtime estimates, calibrated separately for discrete VRAM,
// shared physical RAM, and host-only backends. Not additional model weights.
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

// A GPU-only budget below the minimum does not prove llama.cpp cannot load:
// its automatic layer placement can spill weights to system RAM.
inline int64_t auto_context_cpu_offload_fallback(
    int64_t gpu_tokens, double gpu_free_gib, double physical_free_gib,
    double weights_gib, double kv_bytes_per_token, bool speculative,
    bool unified_gpu, bool allow_cpu_offload, int64_t minimum_ctx = 4096) {
    if (!allow_cpu_offload || gpu_tokens >= minimum_ctx || minimum_ctx <= 0 ||
        !std::isfinite(weights_gib) || weights_gib <= 0.0 ||
        !std::isfinite(kv_bytes_per_token) || kv_bytes_per_token < 0.0)
        return 0;

    // A bounded, pure-SWA cache is already charged to weights_gib. Its
    // remaining per-token slope is zero, not another full window of KV.
    const double minimum_kv_gib = static_cast<double>(minimum_ctx) *
        kv_bytes_per_token / (1024.0 * 1024.0 * 1024.0);
    if (unified_gpu) {
        if (!std::isfinite(physical_free_gib) || physical_free_gib <= 0.0)
            return 0;
        // CPU/GPU share physical RAM; GTT no longer limits CPU-resident weights.
        const auto& policy = AUTO_CTX_UNIFIED_GPU_RESERVE;
        const double required_gib = weights_gib + minimum_kv_gib +
            policy.memory_headroom.estimate(physical_free_gib) +
            policy.compute_workspace.estimate(weights_gib) +
            (speculative ? AUTO_CTX_SPECULATIVE_WORKSPACE_GIB : 0.0);
        return physical_free_gib >= required_gib ? minimum_ctx : 0;
    }

    if (!std::isfinite(physical_free_gib) || physical_free_gib < 0.0)
        return minimum_ctx;
    // Measured GPU VRAM and host RAM are independent pools. Leave headroom in
    // both, and charge a conservative workspace for either placement; the
    // llama.cpp fit algorithm decides the actual GPU/CPU layer split.
    const double gpu_usable_gib = (std::max)(0.0, gpu_free_gib -
        AUTO_CTX_DISCRETE_GPU_RESERVE.memory_headroom.estimate(gpu_free_gib));
    const double ram_usable_gib = (std::max)(0.0, physical_free_gib -
        AUTO_CTX_HOST_RESERVE.memory_headroom.estimate(physical_free_gib));
    const double combined_gib = gpu_usable_gib + ram_usable_gib;
    const double workspace_gib = (std::max)(
        AUTO_CTX_DISCRETE_GPU_RESERVE.compute_workspace.estimate(weights_gib),
        AUTO_CTX_HOST_RESERVE.compute_workspace.estimate(weights_gib));
    const double required_gib = weights_gib + workspace_gib + minimum_kv_gib +
        (speculative ? AUTO_CTX_SPECULATIVE_WORKSPACE_GIB : 0.0);
    return combined_gib >= required_gib ? minimum_ctx : 0;
}

// An explicit placement/disabled --fit means llama.cpp cannot be assumed to
// adjust GPU layers automatically. Do not combine VRAM and host RAM on that
// assumption; users can still request an explicit ctx_size if desired.
inline bool auto_context_llamacpp_can_cpu_offload(
    const std::string& custom_args,
    const char* fit_env = std::getenv("LLAMA_ARG_FIT"),
    const char* gpu_layers_env = std::getenv("LLAMA_ARG_N_GPU_LAYERS")) {
    if ((fit_env && std::string(fit_env) == "off") ||
        (gpu_layers_env && std::string(gpu_layers_env) != "auto"))
        return false;

    const auto args = utils::parse_custom_args(custom_args);
    for (size_t i = 0; i < args.size(); ++i) {
        const auto eq = args[i].find('=');
        const std::string flag = args[i].substr(0, eq);
        const bool fit_flag = flag == "--fit" || flag == "-fit";
        const bool layers_flag = flag == "-ngl" || flag == "--gpu-layers" ||
                                 flag == "--n-gpu-layers";
        if (!fit_flag && !layers_flag) continue;
        std::string value;
        if (eq != std::string::npos) {
            value = args[i].substr(eq + 1);
        } else if (i + 1 < args.size() && !args[i + 1].empty() &&
                   args[i + 1][0] != '-') {
            value = args[++i];
        }
        if ((fit_flag && value == "off") ||
            (layers_flag && value != "auto")) return false;
    }
    return true;
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

// Testable weight-only offload admission, independent of KV-cache size.
// Unknown host RAM is not a positive fit; the caller may still allow a
// best-effort fallback in non-safer mode when hardware measurements are absent.
inline bool auto_context_cpu_offload_weights_fit(
    double gpu_free_gib, double physical_free_gib, double weights_gib,
    bool speculative, bool unified_gpu, bool allow_cpu_offload) {
    if (!allow_cpu_offload || !std::isfinite(weights_gib) || weights_gib <= 0.0 ||
        !std::isfinite(gpu_free_gib)) return false;
    const double extra = speculative ? AUTO_CTX_SPECULATIVE_WORKSPACE_GIB : 0.0;
    if (unified_gpu) {
        if (!std::isfinite(physical_free_gib) || physical_free_gib <= 0.0)
            return false;
        const auto& p = AUTO_CTX_UNIFIED_GPU_RESERVE;
        return physical_free_gib >= weights_gib +
            p.memory_headroom.estimate(physical_free_gib) +
            p.compute_workspace.estimate(weights_gib) + extra;
    }
    if (!std::isfinite(physical_free_gib) || physical_free_gib < 0.0)
        return false;
    const double gpu_usable = (std::max)(0.0, gpu_free_gib -
        AUTO_CTX_DISCRETE_GPU_RESERVE.memory_headroom.estimate(gpu_free_gib));
    const double ram_usable = (std::max)(0.0, physical_free_gib -
        AUTO_CTX_HOST_RESERVE.memory_headroom.estimate(physical_free_gib));
    const double workspace = (std::max)(
        AUTO_CTX_DISCRETE_GPU_RESERVE.compute_workspace.estimate(weights_gib),
        AUTO_CTX_HOST_RESERVE.compute_workspace.estimate(weights_gib));
    return gpu_usable + ram_usable >= weights_gib + workspace + extra;
}

// Pure, deterministic policy: a weight fit does not imply the KV cache fits.
// The ordinary mode tries the useful minimum when only reserve estimates are
// uncertain. A backend-provided KV budget is authoritative: never override it.
inline int64_t auto_context_minimum_admission(
    int64_t estimated_tokens, int64_t minimum_tokens,
    bool weights_fit, bool measured_offloaded_minimum_fit,
    bool unmeasured_host_offload, bool safer,
    bool backend_kv_budget_known = false) {
    if (minimum_tokens <= 0) return 0;
    if (estimated_tokens >= minimum_tokens) return estimated_tokens;
    if (backend_kv_budget_known) return 0;
    if (measured_offloaded_minimum_fit) return minimum_tokens;
    if (safer) return 0;
    return weights_fit || unmeasured_host_offload ? minimum_tokens : 0;
}

} // namespace lemon
