#pragma once

#include "lemon/backends/configured/configured_backend_entry.h"

namespace lemon::backends {

struct BuiltinEntryBuilder {
    static std::shared_ptr<const ConfiguredBackendEntry> make(
        std::shared_ptr<const ExecutableDefinition> executable, const BackendDescriptor& canonical);
};

}
