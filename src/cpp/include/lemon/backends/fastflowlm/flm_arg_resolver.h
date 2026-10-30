#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace backends {

struct FLMArgResolution {
    std::vector<std::string> args;
};

FLMArgResolution resolve_flm_args(const std::string& flm_args, int ctx_size);

} // namespace backends
} // namespace lemon
