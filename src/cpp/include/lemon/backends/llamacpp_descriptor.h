#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The llamacpp backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in llamacpp_descriptor.cpp.
extern const BackendDescriptor llamacpp_descriptor;

} // namespace backends
} // namespace lemon
