#pragma once

#include <string>
#include <vector>

#include "lemon/model_types.h"

namespace lemon {
namespace backends {

struct FLMArgResolution {
    std::vector<std::string> args;
};

FLMArgResolution resolve_flm_args(const std::string& flm_args, int ctx_size);

// Build the managed server command, including validated user arguments.
std::vector<std::string> build_flm_serve_args(ModelType model_type,
                                              const std::string& checkpoint,
                                              int ctx_size, int port,
                                              const FLMArgResolution& resolution);

} // namespace backends
} // namespace lemon
