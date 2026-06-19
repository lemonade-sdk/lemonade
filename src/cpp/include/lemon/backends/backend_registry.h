#pragma once

#include <memory>
#include <string>
#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/backend_descriptor_registry.h"

namespace lemon {

class WrappedServer;
class ModelManager;
class BackendManager;
class CloudProviderRegistry;
struct ModelInfo;

namespace backends {

// Everything a backend's create() needs to build an instance. Mirrors the
// arguments the old router factory passed to each backend constructor.
struct BackendContext {
    std::string log_level;
    ModelManager* model_manager = nullptr;
    BackendManager* backend_manager = nullptr;
    CloudProviderRegistry* cloud_registry = nullptr;
    const ModelInfo* model_info = nullptr;  // for per-create setup (cloud provider, ryzenai model path)
};

using BackendCreateFn = std::unique_ptr<WrappedServer> (*)(const BackendContext&);

// Binds a descriptor (what the backend is) to its server class's create() (how
// it runs). The generated factory registry supplies one per backend. This API is
// server-only: it references server classes via create(), so it is compiled into
// lemond but not the CLI. The CLI reads descriptors through backend_descriptor_registry.h.
struct BackendRegistration {
    const BackendDescriptor* descriptor;
    BackendCreateFn create;
};

// All registered (descriptor, create) pairs, in LEMON_BACKENDS order.
const std::vector<BackendRegistration>& all_registrations();

// Construct a backend instance for a recipe and associate its descriptor, or
// nullptr if the recipe has no registered backend.
std::unique_ptr<WrappedServer> create_server(const std::string& recipe, const BackendContext& ctx);

} // namespace backends
} // namespace lemon
