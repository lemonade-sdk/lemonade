#include "backend_definition_resources.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/onnxruntime/onnxruntime_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/wrapped_server.h"

#include <utility>

namespace lemon::backends::onnxruntime {

namespace {
BuiltinLoadLog load_log() {
    BuiltinLoadLog log;
    log.loading = [](const std::string& category, const std::string& model_name, const RecipeOptions& options) {
        LOG(INFO, category.c_str()) << "Loading model: " << model_name << std::endl;
        LOG(INFO, category.c_str()) << "Per-model settings: " << options.to_log_string() << std::endl;
    };
    log.prepared = [](const std::string& category, const std::string& phase, const json& values) {
        if (phase == "hook") LOG(INFO, category.c_str()) << "Using model: " << values.at("launch").at("hook").get<std::string>() << std::endl;
        if (phase == "program") LOG(INFO, category.c_str()) << "Using executable: " << values.at("launch").at("executable").get<std::string>() << std::endl;
    };
    return log;
}
}

std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:onnxruntime.json", k_onnxruntime_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::LegacyNonChatSse, {DEVICE_CPU, false, load_log()});
    }();
    return entry;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return configured_entry()->factory(ctx); }
const BackendSpec* spec() { return &configured_entry()->spec; }
const BackendOps* ops() { return configured_entry()->ops.get(); }

}
