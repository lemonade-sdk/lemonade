#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The sdcpp backend's factory (constructs the server class — lemond only).
// Defined in sdcpp_factory.cpp.
std::unique_ptr<WrappedServer> sdcpp_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
