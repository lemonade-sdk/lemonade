#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/moonshine/moonshine_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::moonshine {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions& options) {
        LOG(INFO, category.c_str()) << "Loading model: " << model_name << std::endl;
        LOG(INFO, category.c_str()) << "Per-model settings: " << options.to_log_string() << std::endl;
    };
    log.prepared = [](const std::string& category, const std::string& phase, const json& values) {
        const auto& launch = values.at("launch");
        if (phase == "artifacts") LOG(INFO, category.c_str()) << "Using model: " << values.at("artifacts").at("main").get<std::string>() << std::endl;
        if (phase == "program") LOG(INFO, category.c_str()) << "Using executable: " << launch.at("executable").get<std::string>() << std::endl;
        if (phase == "ports") LOG(INFO, category.c_str()) << "Starting server on port " << launch.at("ports").at("http").get<int>() << " (TCP streaming on " << launch.at("ports").at("tcp").get<int>() << ")" << std::endl;
    };
    log.starting = [](const std::string& category, const json& values, const ServerCommand&) {
        const std::string custom = values.at("options").at("moonshine_args");
        if (!custom.empty()) LOG(DEBUG, category.c_str()) << "Adding custom arguments: " << custom << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:moonshine.json", k_moonshine_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::LegacyNonChatSse, {DEVICE_CPU, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
