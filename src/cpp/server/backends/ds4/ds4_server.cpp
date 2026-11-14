#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::ds4 {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.starting = [](const std::string& category, const json& values, const ServerCommand& command) {
        const std::string custom = values.at("options").at("ds4_args");
        if (!custom.empty()) LOG(DEBUG, category.c_str()) << "Adding custom arguments: " << custom << std::endl;
        LOG(INFO, category.c_str()) << "Starting ds4-server (" << command.program << ") for " << values.at("artifacts").at("main").get<std::string>()
                                    << " on port " << command.port << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:ds4.json", k_ds4_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::None, {DEVICE_GPU, true, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
