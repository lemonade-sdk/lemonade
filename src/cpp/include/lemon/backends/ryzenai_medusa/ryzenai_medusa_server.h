#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"

namespace lemon {
namespace backends {
namespace ryzenai_medusa {

// Factory, spec, ops, and capabilities for the ryzenai-llm-medusa backend.
// The server class is RyzenAIServer — reused identically, with a different
// BackendSpec that points to the medusa-server.zip release asset.
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RyzenAIServer>(); }

}  // namespace ryzenai_medusa
}  // namespace backends
}  // namespace lemon
