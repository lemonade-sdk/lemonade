#pragma once

#include <optional>
#include <string>
#include <vector>

#include "lemon/model_types.h"

namespace lemon::mcp {

struct MediaModelCandidate {
    std::string name;
    ModelType type;
    std::vector<std::string> capabilities;
};

bool media_model_eligible(const MediaModelCandidate& candidate,
                          ModelType required_type,
                          const std::string& required_capability = {});

std::optional<std::string> select_media_model(
        const std::vector<MediaModelCandidate>& loaded,
        const std::vector<MediaModelCandidate>& downloaded,
        ModelType required_type,
        const std::string& required_capability = {});

}  // namespace lemon::mcp
