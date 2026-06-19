#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {

// The fastflowlm backend's descriptor (plain data — CLI-safe, links into both the
// lemonade CLI and lemond). Defined in fastflowlm_descriptor.cpp.
extern const BackendDescriptor fastflowlm_descriptor;

} // namespace backends
} // namespace lemon
