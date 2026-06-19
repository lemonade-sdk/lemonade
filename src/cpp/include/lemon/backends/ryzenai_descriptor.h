#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The ryzenai backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in ryzenai_descriptor.cpp.
extern const BackendDescriptor ryzenai_descriptor;

} // namespace backends
} // namespace lemon
