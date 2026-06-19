#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The cloud backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in cloud_descriptor.cpp.
extern const BackendDescriptor cloud_descriptor;

} // namespace backends
} // namespace lemon
