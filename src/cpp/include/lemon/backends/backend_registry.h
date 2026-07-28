#pragma once

#include <memory>
#include <string>
#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_ops.h"

namespace lemon {

class WrappedServer;
class ModelManager;
class BackendManager;
class CloudProviderRegistry;
struct ModelInfo;

namespace backends {

struct BackendSpec;  // install/download spec, defined in backend_utils.h

// Everything a backend's create() needs to build an instance.
struct BackendContext {
    std::string log_level;
    ModelManager* model_manager = nullptr;
    BackendManager* backend_manager = nullptr;
    CloudProviderRegistry* cloud_registry = nullptr;
    const ModelInfo* model_info = nullptr;  // for per-create setup (cloud provider, ryzenai model path)
};

using BackendCreateFn = std::unique_ptr<WrappedServer> (*)(const BackendContext&);

// Convenience for the common create(): construct a server class from the
// standard (log_level, model_manager, backend_manager) context fields.
template <typename T>
std::unique_ptr<WrappedServer> make_server(const BackendContext& ctx) {
    return std::make_unique<T>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

// Construct-on-first-use singleton for a stateless ops class, giving the
// registry a stable pointer. Backends with no custom behavior return
// default_backend_ops() from their ops() instead.
template <typename T>
const BackendOps* single_ops() {
    static const T kOps;
    return &kOps;
}

// Binds a descriptor (what the backend is) to its server class's create() (how
// it runs). The generated factory registry supplies one per backend. This API is
// server-only: it references server classes via create(), so it is compiled into
// lemond but not the CLI. The CLI reads descriptors through backend_descriptor_registry.h.
struct BackendRegistration {
    const BackendDescriptor* descriptor;
    BackendCreateFn create;
    const BackendSpec* spec;  // install/download spec, or nullptr (e.g. cloud has none)
    const BackendOps* ops;    // stateless model-management behavior (never null)
};

// All registered (descriptor, create, spec, ops) entries, in LEMON_BACKENDS order.
const std::vector<BackendRegistration>& all_registrations();

// Install/download spec for a recipe, or nullptr if the recipe has none.
const BackendSpec* spec_for(const std::string& recipe);

// Stateless model-management ops for a recipe. Falls back to the shared default
// ops (base behavior) for recipes with no registered backend.
const BackendOps* ops_for(const std::string& recipe);

// Construct a backend instance for a recipe and associate its descriptor, or
// nullptr if the recipe has no registered backend.
std::unique_ptr<WrappedServer> create_server(const std::string& recipe, const BackendContext& ctx);

} // namespace backends
} // namespace lemon
