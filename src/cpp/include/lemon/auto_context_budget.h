#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lemon {

// Once model artifacts have been resolved, ModelInfo::size includes *all*
// resolved paths (main, mmproj, draft, and sharded GGUF). The old heuristic
// counted mmproj/draft a second time. Use whichever total is more complete.
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

struct AutoContextBudget {
    double model_gib = 0.0;
    double reserve_gib = 0.0;
    double kv_gib = 0.0;
    int64_t max_tokens = 0;
};

inline AutoContextBudget estimate_auto_context_budget(double free_gib,
                                                       double weights_gib,
                                                       double extra_weights_gib,
                                                       double kv_bytes_per_token,
                                                       bool gpu,
                                                       bool speculative,
                                                       bool unified_gpu = false) {
    AutoContextBudget result;
    if (!std::isfinite(free_gib) || !std::isfinite(weights_gib) ||
        !std::isfinite(extra_weights_gib) || !std::isfinite(kv_bytes_per_token) ||
        free_gib <= 0.0 || weights_gib <= 0.0 || extra_weights_gib < 0.0 ||
        kv_bytes_per_token <= 0.0) {
        return result;
    }

    // GGUF sizes already include quantization data and tensor alignment.
    // Runtime allocations belong in reserve_gib, not in a weight surcharge.
    result.model_gib = weights_gib + extra_weights_gib;

    // Estimate *runtime* overhead separately from weights and the KV cache.
    // Clamp GPU headroom: leaving 15% of a 32 GiB card unused needlessly
    // sacrifices ~75k context tokens for a 64 KiB/token KV cache.
    double headroom_gib;
    double workspace_gib;
    if (gpu && !unified_gpu) {
        // Discrete GPU: driver/graph slack plus fixed compute buffers. Bound
        // both terms so high-VRAM cards do not lose many GiB to percentages.
        headroom_gib = std::clamp(free_gib * 0.045, 0.75, 2.0);
        workspace_gib = std::clamp(result.model_gib * 0.05, 0.5, 1.75);
    } else if (gpu) {
        // UMA must retain free physical RAM for the OS and other processes;
        // its available pool has already been capped by system RAM usage.
        headroom_gib = std::clamp(free_gib * 0.08, 2.0, 10.0);
        workspace_gib = std::clamp(result.model_gib * 0.04, 0.5, 3.0);
    } else {
        // Preserve the more cautious system-RAM policy for CPU/NPU paths.
        headroom_gib = (std::max)(1.0, free_gib * 0.05);
        workspace_gib = (std::max)(1.0, result.model_gib * 0.05);
    }

    // Built-in MTP weights are already part of the main GGUF file. The MTP
    // context's extra token-dependent storage is charged by the caller.
    const double draft_workspace_gib = speculative ? 0.25 : 0.0;
    result.reserve_gib = headroom_gib + workspace_gib + draft_workspace_gib;
    result.kv_gib = (std::max)(0.0, free_gib - result.model_gib - result.reserve_gib);

    constexpr double bytes_per_gib = 1024.0 * 1024.0 * 1024.0;
    const double tokens = std::floor(result.kv_gib * bytes_per_gib / kv_bytes_per_token);
    if (tokens > 0.0) {
        result.max_tokens = static_cast<int64_t>(
            (std::min)(tokens, static_cast<double>(std::numeric_limits<int64_t>::max())));
    }
    return result;
}

} // namespace lemon
