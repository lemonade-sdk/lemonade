#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/kokoro/kokoro_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::kokoro {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions&) {
        LOG(INFO, category.c_str()) << "Loading model: " << model_name << std::endl;
    };
    log.prepared = [](const std::string& category, const std::string& phase, const json& values) {
        if (phase == "ports") LOG(INFO, category.c_str()) << "Starting server on port " << values.at("launch").at("ports").at("http").get<int>() << std::endl;
        if (phase != "environment") return;
        for (const auto& entry : values.at("launch").at("environment")) {
            if (entry[0] == "LD_LIBRARY_PATH") LOG(INFO, category.c_str()) << "Setting LD_LIBRARY_PATH=" << entry[1].get<std::string>() << std::endl;
        }
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:kokoro.json", k_kokoro_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::LegacyNonChatSse, {{}, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
