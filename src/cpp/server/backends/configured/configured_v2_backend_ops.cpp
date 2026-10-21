#include "configured_v2_backend_ops_internal.h"
#include "lemon/backends/configured/backend_definition_descriptor.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/configured_server.h"

namespace lemon::backends {

struct ConfiguredBackendEntry::V2FactoryState {
    std::weak_ptr<const ConfiguredBackendEntry> owner;
};
std::shared_ptr<const ConfiguredBackendEntry> ConfiguredBackendEntry::make(std::shared_ptr<const ExecutableDefinition> executable) {
    auto entry = std::shared_ptr<ConfiguredBackendEntry>(new ConfiguredBackendEntry(std::move(executable)));
    entry->v2_factory_state_->owner = entry;
    return entry;
}

ConfiguredBackendEntry::ConfiguredBackendEntry(std::shared_ptr<const ExecutableDefinition> executable)
    : v2_factory_state_(std::make_shared<V2FactoryState>()), runner_(std::make_shared<const DefinitionRunner>(executable)),
      descriptor(descriptor_from_declaration(runner_->declaration(), executable->compiled().authorized().definition().key.space, DescriptorPlatform::native())),
      bindings(runner_->operation_bindings()), v2_ops(std::make_shared<const ConfiguredV2BackendOps>(runner_)),
      executable_definition(std::move(executable)), spec(descriptor.recipe, descriptor.binary, executable_definition),
      compatibility(TrustedCompatibility::None),
      factory([state = v2_factory_state_](const BackendContext& context) -> std::unique_ptr<WrappedServer> {
          auto owner = state->owner.lock();
          if (!owner) throw std::logic_error("Configured factory entry has expired");
          return std::make_unique<ConfiguredServer>(std::move(owner), context);
      }) {}

const json& ConfiguredBackendEntry::records() const {
    return definition ? definition->records : executable_definition->compiled().normalized_declaration();
}


void ConfiguredV2BackendOps::resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const {
    runner_->resolve_runtime_options(model, options);
}
std::string ConfiguredV2BackendOps::resolve_checkpoint_path(const ModelInfo&, const CheckpointResolveContext&) const {
    throw std::logic_error("Configured v2 artifact consumer is unavailable");
}
bool ConfiguredV2BackendOps::is_downloaded(const ModelInfo&, const BackendOpsContext&) const {
    throw std::logic_error("Configured v2 artifact consumer is unavailable");
}
void ConfiguredV2BackendOps::download_model(const ModelInfo&, bool, DownloadProgressCallback, const BackendOpsContext&) const {
    throw std::logic_error("Configured v2 installation consumer is unavailable");
}

}
