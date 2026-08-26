#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model_manager.h"
#include "model_types.h"

namespace lemon {
namespace mcp {

// Lightweight input for deterministic, GPU-free model-selection tests.
struct MediaModelCandidate {
    std::string name;
    ModelInfo info;
};

inline bool model_advertises_capability(const ModelInfo& info,
                                        const std::string& required_capability) {
    if (required_capability.empty()) return true;
    const std::string wanted = normalize_capability_label(required_capability);
    for (const std::string& label : info.labels) {
        if (normalize_capability_label(label) == wanted) return true;
    }
    return false;
}

inline bool media_model_eligible(const ModelInfo& info,
                                 ModelType required_type,
                                 const std::string& required_capability = {}) {
    return info.type == required_type &&
           model_advertises_capability(info, required_capability);
}

inline std::optional<std::string> select_media_model(
        const std::vector<MediaModelCandidate>& loaded,
        const std::vector<MediaModelCandidate>& downloaded,
        ModelType required_type,
        const std::string& required_capability = {}) {
    auto select = [&](const std::vector<MediaModelCandidate>& candidates)
        -> std::optional<std::string> {
        for (const auto& candidate : candidates) {
            if (!candidate.name.empty() &&
                media_model_eligible(candidate.info, required_type, required_capability)) {
                return candidate.name;
            }
        }
        return std::nullopt;
    };

    if (auto loaded_match = select(loaded)) return loaded_match;
    return select(downloaded);
}

}  // namespace mcp
}  // namespace lemon
