#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The vllm backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in vllm_descriptor.cpp.
extern const BackendDescriptor vllm_descriptor;

} // namespace backends
} // namespace lemon
