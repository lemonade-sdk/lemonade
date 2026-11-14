#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/wrapped_server.h"

#include <stdexcept>

namespace lemon::backends::hrx {

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    for (const auto& row : all_registrations())
        if (row.descriptor->recipe == "llamacpp-hrx" && row.configured_entry && row.configured_entry->executable_definition)
            return row.configured_entry->shared_from_this();
    throw std::logic_error("HRX requires finalized builtin authority");
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->backend_ops(); }

}
