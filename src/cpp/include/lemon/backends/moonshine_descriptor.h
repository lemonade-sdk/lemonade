#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The moonshine backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in moonshine_descriptor.cpp.
extern const BackendDescriptor moonshine_descriptor;

} // namespace backends
} // namespace lemon
