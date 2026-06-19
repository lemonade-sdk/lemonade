#pragma once

#include <memory>
#include "lemon/backends/backend_registry.h"

namespace lemon {
namespace backends {

// The vllm backend's factory (constructs the server class — lemond only).
// Defined in vllm_factory.cpp.
std::unique_ptr<WrappedServer> vllm_create(const BackendContext& ctx);

} // namespace backends
} // namespace lemon
