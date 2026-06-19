#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The llamacpp backend's factory (constructs the server class — lemond only).
// Defined in llamacpp_factory.cpp.
std::unique_ptr<WrappedServer> llamacpp_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
