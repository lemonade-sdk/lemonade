#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The fastflowlm backend's factory (constructs the server class — lemond only).
// Defined in fastflowlm_factory.cpp.
std::unique_ptr<WrappedServer> fastflowlm_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
