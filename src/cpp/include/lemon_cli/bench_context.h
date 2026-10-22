#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace lemon_cli {

inline int bench_context_for_target_tokens(int target_tokens) {
    if (target_tokens <= 0) return 0;
    int ctx = 4096;
    while (ctx < target_tokens) {
        if (ctx > std::numeric_limits<int>::max() / 2) return target_tokens;
        ctx *= 2;
    }
    return ctx;
}

struct BenchContextPlan {
    // Group by context so each result contains scenarios actually run at that size.
    std::map<int, std::vector<std::size_t>> batches;
    std::vector<std::pair<int, std::size_t>> skipped;
};

inline BenchContextPlan plan_bench_contexts(const std::vector<int>& requested_sizes,
                                            int model_default_ctx,
                                            const std::vector<int>& required_sizes) {
    BenchContextPlan plan;
    if (requested_sizes.empty()) {
        for (std::size_t i = 0; i < required_sizes.size(); ++i) {
            const int ctx = required_sizes[i] > 0
                ? std::max(model_default_ctx, required_sizes[i]) : model_default_ctx;
            plan.batches[ctx].push_back(i);
        }
    } else {
        for (int ctx : requested_sizes) {
            for (std::size_t i = 0; i < required_sizes.size(); ++i) {
                // Nonpositive values use server defaults; size is not known here.
                if (ctx > 0 && ctx < required_sizes[i])
                    plan.skipped.emplace_back(ctx, i);
                else
                    plan.batches[ctx].push_back(i);
            }
        }
    }
    return plan;
}

}  // namespace lemon_cli
