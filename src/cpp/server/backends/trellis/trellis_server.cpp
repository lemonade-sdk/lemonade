#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/trellis/trellis_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::trellis {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions&) {
        LOG(INFO, category.c_str()) << "Loading model: " << model_name << std::endl;
    };
    log.starting = [](const std::string& category, const json& values, const ServerCommand& command) {
        const std::string custom = values.at("options").at("trellis_args");
        if (!custom.empty()) LOG(DEBUG, category.c_str()) << "Adding custom arguments: " << custom << std::endl;
        LOG(INFO, category.c_str()) << "Starting " << command.program << " on port " << command.port << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:trellis.json", k_trellis_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::LegacyNonChatSse, {{}, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
