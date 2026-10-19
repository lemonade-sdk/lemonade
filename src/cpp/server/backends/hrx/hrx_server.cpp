#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::hrx {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions& options) {
        LOG(INFO, category.c_str()) << "Loading model: " << model_name << std::endl;
        LOG(DEBUG, category.c_str()) << "Per-model settings: " << options.to_log_string() << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:hrx.json", k_hrx_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::None, {{}, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
