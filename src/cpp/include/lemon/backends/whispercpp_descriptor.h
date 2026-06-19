#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The whispercpp backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in whispercpp_descriptor.cpp.
extern const BackendDescriptor whispercpp_descriptor;

} // namespace backends
} // namespace lemon
