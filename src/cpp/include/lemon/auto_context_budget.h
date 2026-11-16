#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

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
    {0.045, 0.75, 2.0}, {0.05, 0.5, 1.75}};
inline constexpr AutoContextReservePolicy AUTO_CTX_UNIFIED_GPU_RESERVE{
    {0.08, 0.75, 10.0}, {0.04, 0.5, 3.0}};
inline constexpr AutoContextReservePolicy AUTO_CTX_HOST_RESERVE{
    {0.05, 1.0, std::numeric_limits<double>::max()},
    {0.05, 1.0, std::numeric_limits<double>::max()}};
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
                                                       bool unified_gpu = false) {
    AutoContextBudget result;
    if (!std::isfinite(free_gib) || !std::isfinite(weights_gib) ||
        !std::isfinite(kv_bytes_per_token) ||
        free_gib <= 0.0 || weights_gib <= 0.0 || kv_bytes_per_token <= 0.0) {
        return result;
    }

    result.model_gib = weights_gib;
    const AutoContextReservePolicy& reserve_policy = !gpu ? AUTO_CTX_HOST_RESERVE
        : unified_gpu ? AUTO_CTX_UNIFIED_GPU_RESERVE : AUTO_CTX_DISCRETE_GPU_RESERVE;
    const double memory_headroom_gib = reserve_policy.memory_headroom.estimate(free_gib);
    const double compute_workspace_gib =
        reserve_policy.compute_workspace.estimate(result.model_gib);
    const double speculative_workspace_gib =
        speculative ? AUTO_CTX_SPECULATIVE_WORKSPACE_GIB : 0.0;
    result.reserve_gib = memory_headroom_gib + compute_workspace_gib +
        speculative_workspace_gib;
    result.kv_gib = (std::max)(0.0, free_gib - result.model_gib - result.reserve_gib);

    constexpr double bytes_per_gib = 1024.0 * 1024.0 * 1024.0;
    const double tokens = std::floor(result.kv_gib * bytes_per_gib / kv_bytes_per_token);
    if (tokens > 0.0) {
        const int64_t limit = std::numeric_limits<int64_t>::max();
        result.max_tokens = tokens >= static_cast<double>(limit)
            ? limit : static_cast<int64_t>(tokens);
    }
    return result;
}

} // namespace lemon
