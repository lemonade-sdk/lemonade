#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/thinksound/thinksound_server.h"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::thinksound {

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:thinksound.json", k_thinksound_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::LegacyNonChatSse);
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
