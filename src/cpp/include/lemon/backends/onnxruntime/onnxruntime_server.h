#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/server_capabilities.h"

#include <cstdint>
#include <memory>

namespace lemon::backends {

struct ConfiguredBackendEntry;

}

namespace lemon::backends::onnxruntime {

std::shared_ptr<const ConfiguredBackendEntry> configured_entry();
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return CAP_CLASSIFICATION; }

}
