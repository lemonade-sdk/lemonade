#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The whispercpp backend's factory (constructs the server class — lemond only).
// Defined in whispercpp_factory.cpp.
std::unique_ptr<WrappedServer> whispercpp_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
