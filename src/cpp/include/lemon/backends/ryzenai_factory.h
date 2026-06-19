#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The ryzenai backend's factory (constructs the server class — lemond only).
// Defined in ryzenai_factory.cpp.
std::unique_ptr<WrappedServer> ryzenai_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
