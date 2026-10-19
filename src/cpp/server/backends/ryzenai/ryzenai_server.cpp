#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::ryzenai {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions&) {
        LOG(DEBUG, category.c_str()) << "Loading model: " << model_name << std::endl;
    };
    log.prepared = [](const std::string& category, const std::string& phase, const json& values) {
        if (phase == "program") LOG(DEBUG, category.c_str()) << "Found ryzenai-server at: " << values.at("launch").at("executable").get<std::string>() << std::endl;
        if (phase == "artifacts") LOG(DEBUG, category.c_str()) << "Model path: " << values.at("artifacts").at("main").get<std::string>() << std::endl;
    };
    log.starting = [](const std::string& category, const json&, const ServerCommand& command) {
        LOG(DEBUG, category.c_str()) << "Starting: \"" << command.program << "\"";
        for (const auto& arg : command.args) {
            LOG(DEBUG, category.c_str()) << " \"" << arg << "\"";
        }
        LOG(DEBUG, category.c_str()) << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:ryzenai.json", k_ryzenai_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::None, {{}, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
