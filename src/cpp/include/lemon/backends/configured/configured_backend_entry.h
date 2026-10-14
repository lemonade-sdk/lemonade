#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/configured/configured_backend_ops.h"

namespace lemon::backends {

struct ConfiguredBackendEntry {
    explicit ConfiguredBackendEntry(std::shared_ptr<const BackendDefinition> definition);
    const std::shared_ptr<const BackendDefinition> definition;
    const BackendDescriptor descriptor;
    const std::shared_ptr<const ConfiguredBackendOps> ops;
    const BackendSpec spec;
};

}
