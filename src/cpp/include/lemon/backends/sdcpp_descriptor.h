#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The sdcpp backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in sdcpp_descriptor.cpp.
extern const BackendDescriptor sdcpp_descriptor;

} // namespace backends
} // namespace lemon
