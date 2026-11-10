#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"

namespace lemon {
namespace backends {
namespace ryzenai_medusa {
// Runs RyzenAIServer against the medusa-server.zip build of ryzenai-server.
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RyzenAIServer>(); }
}  // namespace ryzenai_medusa
}  // namespace backends
}  // namespace lemon
