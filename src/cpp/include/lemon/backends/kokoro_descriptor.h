#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The kokoro backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in kokoro_descriptor.cpp.
extern const BackendDescriptor kokoro_descriptor;

} // namespace backends
} // namespace lemon
