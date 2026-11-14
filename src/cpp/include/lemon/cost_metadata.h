#pragma once

#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

inline constexpr const char* kCostTierKey = "cost_tier";
inline constexpr const char* kCostInputPerMillionKey = "cost_input_per_million";
inline constexpr const char* kCostOutputPerMillionKey = "cost_output_per_million";
inline constexpr const char* kLatencyMsHintKey = "latency_ms_hint";

// ModelInfo has no typed field for these hand-authored hints, so they ride in
// ModelInfo::extras and every allowlist that filters model JSON must name them.
inline constexpr std::array<const char*, 4> kCostMetadataKeys = {
    kCostTierKey, kCostInputPerMillionKey, kCostOutputPerMillionKey, kLatencyMsHintKey};

inline bool is_cost_tier(const std::string& tier) {
    return tier == "free" || tier == "low" || tier == "medium" || tier == "high";
}

// The cost keys in `entry` that routing will ignore: a cost_tier outside
// free|low|medium|high, or a price or latency that is not a non-negative number.
// A null value counts as unset, not unusable.
inline std::vector<std::string> unusable_cost_metadata(const nlohmann::json& entry) {
    std::vector<std::string> unusable;
    if (!entry.is_object()) {
        return unusable;
    }
    for (const char* key : kCostMetadataKeys) {
        const auto it = entry.find(key);
        if (it == entry.end() || it->is_null()) {
            continue;
        }
        const bool usable = std::string(key) == kCostTierKey
                                ? it->is_string() && is_cost_tier(it->get<std::string>())
                                : it->is_number() && it->get<double>() >= 0.0;
        if (!usable) {
            unusable.emplace_back(key);
        }
    }
    return unusable;
}

} // namespace lemon
