#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The kokoro backend's factory (constructs the server class — lemond only).
// Defined in kokoro_factory.cpp.
std::unique_ptr<WrappedServer> kokoro_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
